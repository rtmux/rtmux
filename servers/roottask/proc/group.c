/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"

/* Serializes every change of a process's group or session (setpgid here, setsid
 * in session.c), so that a check made about a group or a session still holds
 * when the change is applied. It is the outermost lock of those paths: taken
 * before any process slot lock, and never while one is held.
 */
static pthread_mutex_t pgrp_mutex = PTHREAD_MUTEX_INITIALIZER;

void proc_pgrp_lock(void)
{
    pthread_mutex_lock(&pgrp_mutex);
}

void proc_pgrp_unlock(void)
{
    pthread_mutex_unlock(&pgrp_mutex);
}

/* What setpgid needs to know about one process, read under its own lock. */
struct pg_snap {
    pid_t parent;
    pid_t sid; /* its session's leader: the system session's, if it has none */
    unsigned flags;
};

static int pg_snapshot(pid_t pid, struct pg_snap *s)
{
    root_proc_info_t qinfo;

    if (!root_proc_lock(pid))
        return ESRCH;
    if (process_query_info(pid, &qinfo) != EOK ||
        (qinfo.flags & RTMUX_FLG_PROC_WAITDONE)) {
        root_proc_unlock(pid);
        return ESRCH;
    }
    s->parent = qinfo.parent_pid;
    s->sid = qinfo.sess_leader;
    s->flags = qinfo.flags;
    root_proc_unlock(pid);
    return EOK;
}

/* The checks and the change of one setpgid, with pgrp_mutex held. At most one
 * process slot is locked at a time, so the lock order of the rest of proc
 * (child, then parent) is never in play; nothing else can move a process
 * between groups or sessions while pgrp_mutex is held, so the sessions and the
 * group read here are the ones the change applies to.
 */
static int setpgid_locked(pid_t caller, pid_t target, pid_t pgid)
{
    struct pg_snap self, tgt;
    pid_t gsid;

    if (pg_snapshot(caller, &self) != EOK)
        return EL2HLT;
    if (target == caller) {
        tgt = self;
    } else {
        /* Only the caller's own children. RTMux spawns rather than forks, and
         * exec runs as a spawn of its own, so the EACCES for a child that has
         * exec'd has nothing to test: a spawned child is movable until it
         * exits.
         */
        if (pg_snapshot(target, &tgt) != EOK || tgt.parent != caller)
            return ESRCH;
    }

    /* A session leader may not change group, and the target must be in the
     * caller's session. A process with no session is in the system session, as
     * for getsid.
     */
    if ((tgt.flags & RTMUX_FLG_PROC_SLEADER) || tgt.sid != self.sid)
        return EPERM;

    /* pgid 0 means the target's own pid: a group of its own. Any other group
     * must have a live member in the caller's session, whether or not the
     * process it is named for is still there.
     */
    if (pgid == 0)
        pgid = target;
    if (pgid != target &&
        (proc_pgrp_session(pgid, &gsid) != EOK || gsid != self.sid))
        return EPERM;

    if (!root_proc_lock(target))
        return ESRCH;
    root_proc(target)->pgrp = pgid;
    root_proc_unlock(target);
    return EOK;
}

/* proc_setpgid - handle a _PROC_SETPGID message (setpgid()).
 *
 * Moves the target process into the requested process group, enforcing the
 * POSIX rules (target must be the caller or one of its children in the same
 * session, and the destination group must have a member in that session).
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = calling process)
 *   msg : the setpgid request (i.pid target, i.pgid new group)
 * Return value: EOK / an errno, or a IOSERV_* status code.
 */
int proc_setpgid(ioserv_context_t *ctp, proc_setpgid_t *msg)
{
    pid_t target = msg->i.pid ? (pid_t) msg->i.pid : ctp->info.pid;
    int ret;

    /* If pgid is less than zero return EINVAL */
    if (msg->i.pgid < 0)
        return EINVAL;

    proc_pgrp_lock();
    ret = setpgid_locked(ctp->info.pid, target, (pid_t) msg->i.pgid);
    proc_pgrp_unlock();
    return ret;
}
