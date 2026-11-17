/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <devctl/tty.h>
#include <sys/procmsg.h>

/* setsid, with pgrp_mutex held: no group or session changes under it. */
static int session_setsid(ioserv_context_t *ctp)
{
    root_proc_info_t qinfo;
    root_process_t *tmp = root_proc(ctp->info.pid);
    session_t *sep;

    /* POSIX: EPERM when the caller's pid names a process group, even one whose
     * only members are others now. Groups are their members
     * (proc_pgrp_session), so this is not "the caller's own group id equals its
     * pid". Asked before the caller's slot is locked: the scan locks each slot
     * in turn.
     */
    if (proc_pgrp_session(ctp->info.pid, NULL) == EOK) {
        return EPERM;
    }

    if (!root_proc_lock(ctp->info.pid)) {
        return EL2HLT;
    }
    if (process_query_info(ctp->info.pid, &qinfo) != EOK) {
        return proc_error(ESRCH, ctp->info.pid);
    }
    /* Must not already be a session leader */
    if (qinfo.flags & RTMUX_FLG_PROC_SLEADER) {
        return proc_error(EPERM, ctp->info.pid);
    }

    /* Session struct lives in roottask heap end-to-end now. */
    if (!(sep = malloc(sizeof *sep))) {
        return proc_error(ENOMEM, ctp->info.pid);
    }
    memset(sep, 0, sizeof(*sep));
    sep->links = 1;
    sep->leader = ctp->info.pid;
    sep->pgrp = sep->fd = -1;
    if (tmp->session && atomic_sub_value(&tmp->session->links, 1) == 1) {
        /* This should only happen if the session leader died... */
        free(tmp->session);
    }
    (void) ProcessFlagsOp(ctp->info.pid, RTMUX_FLG_PROC_SLEADER, 0);
    tmp->session = sep;
    ctp->status = tmp->pgrp = ctp->info.pid;

    root_proc_unlock(ctp->info.pid);
    return EOK;
}

/* proc_msg_session — handle a _PROC_SESSION message.
 *
 * Dispatches the session events: TCSETSID (assign / drop a controlling
 * terminal), SETSID (create a new session), and SETPGRP (move a process into a
 * process group).
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = calling process)
 *   msg : the session request (i.event selects the operation)
 * Return value: EOK / an errno, or a IOSERV_* status code.
 */
int proc_msg_session(ioserv_context_t *ctp, proc_session_t *msg)
{
    process_t *prp;
    session_t *sep;

    switch (msg->i.event) {
    case _PROC_SESSION_TCSETSID: {
        io_openfd_t io_openfd;
        int fd;
        int status;
        root_proc_info_t qinfo;

        if (!(prp = root_proc_lock(ctp->info.pid))) {
            return EL2HLT;
        }

        /* prp->{flags,session} live in the kernel-allocated process_t; U-mode
         * roottask can't deref them. Snapshot via the kernel extension — the
         * session pointer itself is safe to deref because session_t is a
         * libc-malloc'd roottask-owned object.
         */
        if (process_query_info(ctp->info.pid, &qinfo) != EOK) {
            return proc_error(ESRCH, ctp->info.pid);
        }
        sep = qinfo.session;
        if (!(qinfo.flags & RTMUX_FLG_PROC_SLEADER) || !sep ||
            msg->i.sid != sep->leader) {
            return proc_error(EPERM, ctp->info.pid);
        }

        if (proc_thread_pool_reserve() != 0) {
            return proc_error(EAGAIN, ctp->info.pid);
        }

        if (msg->i.id == -1) {
            pid_t none = 0;

            fd = sep->fd;
            sep->fd = -1;
            root_proc_unlock(ctp->info.pid);

            /* Tell the terminal server it has no session now, so that tcgetsid
             * on a descriptor that is still open stops naming one. A server
             * that is gone cannot answer, and has nothing to clear.
             */
            if (fd >= 0) {
                (void) _devctl(fd, DEVCTL_TTY_SET_SID, &none, sizeof none, 0);
                close(fd);
            }
            proc_thread_pool_reserve_done();
            return EOK;
        }

        /* We know the process won't die because we haven't replied so sep stays
         * valid
         */
        root_proc_unlock(ctp->info.pid);

        memset(&io_openfd.i, 0x00, sizeof io_openfd.i);

        /* If we have a controlling terminal, and the managing server is still
         * alive then error out can't have two controlling terminals. If we
         * detect the server has disappeared, the set sep->fd = -1;
         */
        if (sep->fd != -1) {
            if (connect_server_info(0, (uint64_t) sep->fd, &io_openfd.i.info) ==
                sep->fd) {
                proc_thread_pool_reserve_done();
                return EPERM;
            } else {
                fd = sep->fd;
                sep->fd = -1;
                close(fd);
            }
        }
        /* Get information on passed fd */
        if (connect_server_info(ctp->info.pid, (uint64_t) msg->i.id,
                                &io_openfd.i.info) != msg->i.id) {
            proc_thread_pool_reserve_done();
            return EBADF;
        }
        /* make a connection to the channel */
        if ((fd = connect_attach(io_openfd.i.info.nd, io_openfd.i.info.pid,
                                 io_openfd.i.info.chid, 0, 0)) == -1) {
            proc_thread_pool_reserve_done();
            return EINVAL;
        }
        /* A terminal is the controlling terminal of one session at a time. */
        {
            struct _server_info srv;

            if (connect_server_info(0, (uint64_t) fd, &srv) != fd ||
                proc_jobctl_tty_busy(srv.pid, sep->leader)) {
                connect_detach_r((uint64_t) fd);
                proc_thread_pool_reserve_done();
                return EPERM;
            }
        }
        /* send the openfd message (no share flags) */
        io_openfd.i.type = _IO_OPENFD;
        io_openfd.i.combine_len = sizeof io_openfd.i;
        io_openfd.i.ioflag = _IO_FLAG_RD | _IO_FLAG_WR;
        io_openfd.i.sflag = 0;
        io_openfd.i.xtype = _IO_OPENFD_NONE;
        io_openfd.i.info.nd = ctp->info.srcnd;
        io_openfd.i.info.pid = ctp->info.pid;
        io_openfd.i.info.coid = msg->i.id;

        /* ipc-send: roottask-fsio -> romfs,lkl-fsd,dev-nvme,pipe */
        if (msg_sendnc(fd, &io_openfd.i, sizeof io_openfd.i, 0, 0) == -1) {
            connect_detach_r((uint64_t) fd);
            proc_thread_pool_reserve_done();
            return errno;
        }

        if (_devctl(fd, DEVCTL_TTY_SET_SID, &sep->leader, sizeof sep->leader,
                    0) == -1) {
            status = errno;
            close(fd);
            proc_thread_pool_reserve_done();
            return status;
        }
        /* unreachable code if(sep->fd != -1) { close(sep->fd); } */
        sep->fd = fd;
        sep->pgrp = sep->leader; /* Posix.1 7.1.1.3: the leader's group leads */
        proc_thread_pool_reserve_done();
        return EOK;
    }

    case _PROC_SESSION_SETSID: {
        int rc;

        proc_pgrp_lock();
        rc = session_setsid(ctp);
        proc_pgrp_unlock();
        return rc;
    }

    case _PROC_SESSION_SETPGRP: {
        pid_t sid_pid = (pid_t) msg->i.sid;
        pid_t id_pid = (pid_t) msg->i.id;
        root_proc_info_t qinfo;
        pid_t leader, gsid;

        if (!proc_isaccess(0, &ctp->info)) {
            break;
        }
        if (!root_proc_lock(sid_pid) ||
            process_query_info(sid_pid, &qinfo) != EOK ||
            !(qinfo.flags & RTMUX_FLG_PROC_SLEADER) ||
            !(sep = root_proc(sid_pid)->session) || sep->fd < 0) {
            return proc_error(EINVAL, sid_pid);
        }
        leader = root_session_leader(root_proc(sid_pid));
        root_proc_unlock(sid_pid);

        /* The group, named by id_pid, is its members: the process it is named
         * for may be gone. The leader's own group (id equal to sid) is the
         * leader's group.
         */
        if (sid_pid != id_pid) {
            if (proc_pgrp_session(id_pid, &gsid) != EOK)
                return EINVAL;
            if (gsid != leader)
                return EPERM;
        }

        /* The session outlives the unlock only while its leader does; look it
         * up again, under the lock, to record the group.
         */
        if (!root_proc_lock(sid_pid))
            return EINVAL;
        sep = root_proc(sid_pid)->session;
        if (sep && sep->fd >= 0)
            sep->pgrp = sid_pid == id_pid
                            ? root_process_pgrp(root_proc(sid_pid))
                            : id_pid;
        root_proc_unlock(sid_pid);
        return EOK;
    }

    case _PROC_SESSION_VALIDATE_PGRP: {
        pid_t pgrp = (pid_t) msg->i.id;
        pid_t gsid;
        root_proc_info_t qinfo;

        if (pgrp <= 0)
            return EINVAL;

        if (!root_proc_lock(ctp->info.pid))
            return EPERM;
        if (process_query_info(ctp->info.pid, &qinfo) != EOK) {
            root_proc_unlock(ctp->info.pid);
            return EPERM;
        }
        root_proc_unlock(ctp->info.pid);

        /* A group exists while it has a member, and must be in the caller's
         * session.
         */
        if (proc_pgrp_session(pgrp, &gsid) != EOK || gsid != qinfo.sess_leader)
            return EPERM;
        return EOK;
    }

    default:
        break;
    }

    return ENOSYS;
}
