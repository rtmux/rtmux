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
static int session_setsid(ioserv_context_t *ctp, session_t **old)
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

    /* The group the caller leads from now on; pgrp_mutex keeps another from
     * taking the id between the check above and this.
     */
    if (proc_pgrp_new(ctp->info.pid) != EOK) {
        free(sep);
        return proc_error(EPERM, ctp->info.pid);
    }
    memset(sep, 0, sizeof(*sep));
    sep->links = 1;
    sep->leader = ctp->info.pid;
    sep->pgrp = sep->fd = -1;
    *old = tmp->session; /* the caller drops its link, outside pgrp_mutex */
    (void) ProcessFlagsOp(ctp->info.pid, RTMUX_FLG_PROC_SLEADER, 0);
    tmp->session = sep;
    ctp->status = ctp->info.pid;

    root_proc_unlock(ctp->info.pid);
    return EOK;
}

/* A binding is reserved by server pid before sending OPENFD. Keep it on this
 * list through the final unbind reply, even after the last process slot has
 * disappeared. Only short list operations hold the mutex.
 */
static pthread_mutex_t tty_bind_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t tty_changed = PTHREAD_COND_INITIALIZER;
static session_t *tty_sessions;

void proc_tty_init(void)
{
    root_mutex_init_pi(&tty_bind_mutex);
}

static void tty_unlist(session_t *sep)
{
    session_t **link = &tty_sessions;
    while (*link && *link != sep)
        link = &(*link)->tty_next;
    if (*link)
        *link = sep->tty_next;
    sep->tty_next = NULL;
    sep->tty_server = 0;
}

/* One queued operation per session permits a normal detach followed by a
 * rebind, without parking the dispatch pool behind an unresponsive server. A
 * reference held by the caller keeps sep alive across the timed wait.
 */
static int tty_claim(session_t *sep, pid_t server)
{
    struct timespec deadline;
    int rc = EOK;

    pthread_mutex_lock(&tty_bind_mutex);
    if (sep->tty_changing) {
        if (sep->tty_waiter || clock_gettime(CLOCK_REALTIME, &deadline) < 0) {
            rc = EBUSY;
            goto out;
        }
        deadline.tv_sec++;
        sep->tty_waiter = true;
        while (sep->tty_changing && rc == EOK)
            rc = pthread_cond_timedwait(&tty_changed, &tty_bind_mutex,
                                        &deadline);
        sep->tty_waiter = false;
        if (rc != EOK)
            goto out;
    }
    if (server) {
        if (sep->fd != -1) {
            struct _server_info info;
            if (connect_server_info(0, (uint64_t) sep->fd, &info) == sep->fd) {
                rc = EPERM;
                goto out;
            }
            (void) connect_detach_r((uint64_t) sep->fd);
            sep->fd = -1;
            tty_unlist(sep);
        }
        for (session_t *other = tty_sessions; other; other = other->tty_next) {
            if (other->tty_server == server) {
                rc = EPERM;
                goto out;
            }
        }
        sep->tty_server = server;
        sep->tty_next = tty_sessions;
        tty_sessions = sep;
    }
    sep->tty_changing = true;
out:
    pthread_mutex_unlock(&tty_bind_mutex);
    return rc;
}

static void tty_finish(session_t *sep)
{
    pthread_mutex_lock(&tty_bind_mutex);
    if (sep->tty_server && sep->fd == -1)
        tty_unlist(sep);
    sep->tty_changing = false;
    if (sep->tty_waiter)
        pthread_cond_broadcast(&tty_changed);
    pthread_mutex_unlock(&tty_bind_mutex);
}

static void tty_release(int fd, bool reserved)
{
    pid_t none = 0;

    if (!reserved && proc_thread_pool_reserve() != 0) {
        /* Neither devctl nor close can self-send without a spare worker. */
        (void) connect_detach_r((uint64_t) fd);
        return;
    }
    (void) _devctl(fd, DEVCTL_TTY_SET_SID, &none, sizeof none, 0);
    close(fd);
    if (!reserved)
        proc_thread_pool_reserve_done();
}

/* Drop one link on a session. The last drop lets go of the terminal, if one is
 * bound, while the session still holds it, so that a session that binds the
 * terminal next is not unbound by this one's message, and frees the session. No
 * lock held.
 */
void proc_session_unref(session_t *sep)
{
    if (atomic_sub_value(&sep->links, 1) != 1)
        return;
    if (sep->fd >= 0) {
        int fd = sep->fd;
        sep->fd = -1;
        tty_release(fd, false);
        tty_finish(sep);
    }
    free(sep);
}

/* TCSETSID: bind the caller's session to the terminal on descriptor id, or (id
 * -1) let it go.
 */
static int session_tcsetsid(ioserv_context_t *ctp, proc_session_t *msg)
{
    io_openfd_t io_openfd;
    session_t *sep;
    pid_t sid, pgid;
    int fd;
    int status = EOK;
    root_proc_info_t qinfo;

    /* Pin the session while the group lock excludes setsid and the slot lock
     * still protects the reference from a departing process.
     */
    proc_pgrp_lock();
    sep = root_session_link_of(ctp->info.pid, &sid, &pgid);
    if (!sep || sid != ctp->info.pid || sid != msg->i.sid) {
        status = EPERM;
    } else if (!root_proc_lock(ctp->info.pid)) {
        status = ESRCH;
    } else {
        status = process_query_info(ctp->info.pid, &qinfo);
        if (status == EOK && !(qinfo.flags & RTMUX_FLG_PROC_SLEADER))
            status = EPERM;
        root_proc_unlock(ctp->info.pid);
    }
    proc_pgrp_unlock();
    if (status != EOK)
        goto unref;
    if (proc_thread_pool_reserve() != 0) {
        status = EAGAIN;
        goto unref;
    }

    memset(&io_openfd.i, 0, sizeof io_openfd.i);
    if (msg->i.id == -1) {
        status = tty_claim(sep, 0);
        if (status != EOK)
            goto done;
        fd = sep->fd;
        sep->fd = -1;
        proc_pgrp_window(PGRP_WINDOW_DETACH);
        if (fd >= 0)
            tty_release(fd, true);
        goto finish;
    }

    if (connect_server_info(ctp->info.pid, (uint64_t) msg->i.id,
                            &io_openfd.i.info) != msg->i.id) {
        status = EBADF;
        goto done;
    }
    status = tty_claim(sep, io_openfd.i.info.pid);
    if (status != EOK)
        goto done;
    fd = connect_attach(io_openfd.i.info.nd, io_openfd.i.info.pid,
                        io_openfd.i.info.chid, 0, 0);
    if (fd == -1) {
        status = errno;
        goto finish;
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
        status = errno;
        connect_detach_r((uint64_t) fd);
        goto finish;
    }

    if (_devctl(fd, DEVCTL_TTY_SET_SID, &sep->leader, sizeof sep->leader, 0) ==
        -1) {
        status = errno;
        close(fd);
        goto finish;
    }
    /* The foreground group is what tcsetpgrp records, under pgrp_mutex. */
    proc_pgrp_lock();
    sep->fd = fd;
    sep->pgrp = sep->leader; /* Posix.1 7.1.1.3: the leader's group leads */
    proc_pgrp_unlock();
finish:
    tty_finish(sep);
done:
    proc_thread_pool_reserve_done();
unref:
    if (sep)
        proc_session_unref(sep);
    return status;
}

/* SETPGRP: record group id as the foreground group of the session led by sid,
 * for a terminal server. pgrp_mutex held, so that the group cannot lose its
 * last member to another session between the check and the record.
 */
static int session_setpgrp(ioserv_context_t *ctp, proc_session_t *msg)
{
    pid_t sid_pid = (pid_t) msg->i.sid;
    pid_t id_pid = (pid_t) msg->i.id;
    root_proc_info_t qinfo;
    session_t *sep;
    pid_t leader, gsid;

    if (!proc_isaccess(0, &ctp->info)) {
        return ENOSYS;
    }
    /* A failed lock holds nothing, so it must not go through proc_error. */
    if (!root_proc_lock(sid_pid)) {
        return EINVAL;
    }
    if (process_query_info(sid_pid, &qinfo) != EOK ||
        !(qinfo.flags & RTMUX_FLG_PROC_SLEADER) ||
        !(sep = root_proc(sid_pid)->session) || sep->fd < 0) {
        return proc_error(EINVAL, sid_pid);
    }
    leader = root_session_leader(root_proc(sid_pid));
    root_proc_unlock(sid_pid);

    /* The group, named by id_pid, is its members: the process it is named for
     * may be gone. The leader's own group (id equal to sid) is the leader's
     * group.
     */
    if (sid_pid != id_pid) {
        if (proc_pgrp_session(id_pid, &gsid) != EOK)
            return EINVAL;
        if (gsid != leader)
            return EPERM;
    }

    /* The session outlives the unlock only while its leader does; look it up
     * again, under the lock, to record the group.
     */
    if (!root_proc_lock(sid_pid))
        return EINVAL;
    sep = root_proc(sid_pid)->session;
    if (sep && sep->fd >= 0)
        sep->pgrp =
            sid_pid == id_pid ? root_process_pgrp(root_proc(sid_pid)) : id_pid;
    root_proc_unlock(sid_pid);
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
    switch (msg->i.event) {
    case _PROC_SESSION_TCSETSID:
        return session_tcsetsid(ctp, msg);

    case _PROC_SESSION_SETSID: {
        session_t *old = NULL;
        int rc;

        proc_pgrp_lock();
        rc = session_setsid(ctp, &old);
        proc_pgrp_unlock();
        if (old)
            proc_session_unref(old);
        return rc;
    }

    case _PROC_SESSION_SETPGRP: {
        int rc;

        proc_pgrp_lock();
        rc = session_setpgrp(ctp, msg);
        proc_pgrp_unlock();
        return rc;
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
