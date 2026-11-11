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
            fd = sep->fd;
            sep->fd = -1;
            close(fd);
            proc_thread_pool_reserve_done();
            return proc_error(EOK, ctp->info.pid);
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

    case _PROC_SESSION_SETSID:
        if (!(prp = root_proc_lock(ctp->info.pid))) {
            return EL2HLT;
        }
        {
            root_proc_info_t qinfo;
            root_process_t *tmp = root_proc(ctp->info.pid);

            if (process_query_info(ctp->info.pid, &qinfo) != EOK) {
                return proc_error(ESRCH, ctp->info.pid);
            }
            /* Must not already be a session or process group leader */
            if ((qinfo.flags & RTMUX_FLG_PROC_SLEADER) ||
                tmp->pgrp == ctp->info.pid) {
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
            if (tmp->session &&
                atomic_sub_value(&tmp->session->links, 1) == 1) {
                /* This should only happen if the session leader died... */
                free(tmp->session);
            }
            (void) ProcessFlagsOp(ctp->info.pid, RTMUX_FLG_PROC_SLEADER, 0);
            tmp->session = sep;
            ctp->status = tmp->pgrp = ctp->info.pid;
        }

        root_proc_unlock(ctp->info.pid);
        return EOK;

    case _PROC_SESSION_SETPGRP: {
        process_t *prp2;
        int ret;
        pid_t leader;
        pid_t sid_pid = (pid_t) msg->i.sid;
        pid_t id_pid = (pid_t) msg->i.id;
        root_proc_info_t qinfo;

        if (!proc_isaccess(0, &ctp->info)) {
            break;
        }
        if (!(prp = root_proc_lock(sid_pid)) ||
            process_query_info(sid_pid, &qinfo) != EOK ||
            !(qinfo.flags & RTMUX_FLG_PROC_SLEADER) ||
            !(sep = root_proc(sid_pid)->session) || sep->fd < 0) {
            return proc_error(EINVAL, sid_pid);
        }

        ret = EOK;
        prp2 = NULL;
        leader = sep->leader;
        if (msg->i.sid == msg->i.id) {
            sep->pgrp = root_proc(sid_pid)->pgrp;
            root_proc_unlock(sid_pid);
        } else {
            root_proc_unlock(sid_pid);
            /* Don't access "sep" pointer from above past the unlock */
            if (!(prp2 = root_proc_lock(id_pid))) {
                ret = EINVAL;
            } else {
                root_process_t *tmp2 = root_proc(id_pid);
                if (id_pid != tmp2->pgrp || !tmp2->session) {
                    ret = EINVAL;
                } else if (tmp2->session->leader != leader) {
                    ret = EPERM;
                } else {
                    tmp2->session->pgrp = tmp2->pgrp;
                }
            }
        }
        if (prp2)
            root_proc_unlock(id_pid);
        return ret;
    }

    case _PROC_SESSION_VALIDATE_PGRP: {
        process_t *target;
        pid_t pgrp = (pid_t) msg->i.id;
        pid_t leader;
        root_proc_info_t qinfo;

        if (pgrp <= 0)
            return EINVAL;

        prp = root_proc_lock(ctp->info.pid);
        if (!prp)
            return EPERM;
        if (process_query_info(ctp->info.pid, &qinfo) != EOK ||
            !root_proc(ctp->info.pid)->session) {
            root_proc_unlock(ctp->info.pid);
            return EPERM;
        }
        leader = root_proc(ctp->info.pid)->session->leader;
        root_proc_unlock(ctp->info.pid);

        target = root_proc_lock(pgrp);
        if (!target) {
            return EPERM;
        }
        if (root_proc(pgrp)->pgrp != pgrp || !root_proc(pgrp)->session) {
            root_proc_unlock(pgrp);
            return EPERM;
        }
        if (root_proc(pgrp)->session->leader != leader) {
            root_proc_unlock(pgrp);
            return EPERM;
        }
        root_proc_unlock(pgrp);
        return EOK;
    }

    default:
        break;
    }

    return ENOSYS;
}
