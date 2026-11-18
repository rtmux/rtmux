/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include "pgrp-table.h"

/* Two locks, in this order and below every other roottask lock but the slot
 * locks of root_proc_lock:
 *
 *   pgrp_mutex   serializes every change of a process's group or session
 *                (setpgid and setsid here and in session.c, and the inheritance
 *                at spawn), so that a check made about a group or a session
 *                still holds when the change is applied. It is the outermost
 *                lock of those paths: taken before any process slot lock, and
 *                never while one is held. A terminal's bind and unbind
 *                (session.c) have a lock of their own, outside this one.
 *   pgtab_lock   the group table and the pgrp, pgrp_gen and pgrp_member of the
 *                slots. A leaf: nothing is taken under it. A reap leaves its
 *                group under it with slot locks held, which is why pgrp_mutex
 *                cannot be what protects the count.
 *
 * Both inherit priority, as a real-time client can wait on either.
 *
 * A group is its members, a zombie included, until the last is reaped: a reap
 * (the waiter's collect, or the teardown of a child nobody waits for) leaves
 * it, and an exit does not. So the member leaving, and a process joining, are
 * each one step under pgtab_lock that also reads whether the group is still
 * there: setpgid cannot join a group whose last member is reaped between its
 * check and its change, and cannot move a process that is reaped between them.
 */
static pthread_mutex_t pgrp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pgtab_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pgrp_table pgtab;

void proc_pgrp_lock(void)
{
    pthread_mutex_lock(&pgrp_mutex);
}

void proc_pgrp_unlock(void)
{
    pthread_mutex_unlock(&pgrp_mutex);
}

#ifdef CONFIG_DEBUG_INJECT
/* A window a test armed holds the next operation that reaches it for the armed
 * time, once, so that another operation can be started inside it.
 */
static atomic_uint pgrp_window_where;
static atomic_uint pgrp_window_ms;

int proc_pgrp_window_arm(unsigned where, unsigned ms)
{
    if (where > PGRP_WINDOW_DETACH || ms > 5000)
        return EINVAL;
    atomic_store(&pgrp_window_ms, ms);
    atomic_store(&pgrp_window_where, where);
    return EOK;
}

void proc_pgrp_window(unsigned where)
{
    unsigned ms;

    if (atomic_load(&pgrp_window_where) != where ||
        atomic_exchange(&pgrp_window_where, PGRP_WINDOW_NONE) != where)
        return;
    ms = atomic_load(&pgrp_window_ms);
    (void) nanosleep(
        &(struct timespec){.tv_sec = ms / 1000,
                           .tv_nsec = (long) (ms % 1000) * 1000000L},
        NULL);
}
#endif

/* proc_pgrp_session - the session group pgrp is in: EOK with *sid set (if not
 * NULL) while the group has a member, a zombie included, or ESRCH. A group is
 * its members, not its leader's pid: it outlives a leader that exited, was
 * reaped or left it.
 */
int proc_pgrp_session(pid_t pgrp, pid_t *sid)
{
    int rc;

    pthread_mutex_lock(&pgtab_lock);
    rc = pgrp_table_find(&pgtab, pgrp, sid);
    pthread_mutex_unlock(&pgtab_lock);
    return rc;
}

/* Count process pid in group pgid of session sid, for the first time. The slot
 * is not yet reachable (a new process), or is roottask's own at boot.
 */
static int pgrp_first_enter(pid_t pid, pid_t pgid, pid_t sid)
{
    root_process_t *p = root_proc(pid);
    uint32_t gen;
    int rc;

    pthread_mutex_lock(&pgtab_lock);
    rc = pgrp_table_enter(&pgtab, pgid, sid, PGRP_JOIN_OR_CREATE, &gen);
    if (rc == EOK) {
        p->pgrp = pgid;
        p->pgrp_gen = gen;
        p->pgrp_member = 1;
    }
    pthread_mutex_unlock(&pgtab_lock);
    return rc;
}

/* Roottask's own group, which its session leads. Make the group locks, and the
 * terminal bind lock, priority-inheriting and empty the table first: boot,
 * before the first process exists.
 */
int proc_pgrp_boot(pid_t pid)
{
    root_mutex_init_pi(&pgrp_mutex);
    root_mutex_init_pi(&pgtab_lock);
    pgrp_table_init(&pgtab);
    proc_tty_init();
    return pgrp_first_enter(pid, pid, pid);
}

/* A new process starts in its parent's group and session: it takes one link on
 * the session (root_process_fini drops it) and is counted in the group. A
 * process with no parent leads a group of its own, in the system session. Under
 * pgrp_mutex, so that the parent cannot change group or session between the two
 * reads, which would count the child in a group its parent has left, or take a
 * link on a session the parent just dropped.
 *
 * Returns EOK, or the errno of the failed count (the table is as large as the
 * process table, so only a corrupt slot fails).
 */
int proc_pgrp_inherit(pid_t child, pid_t parent)
{
    root_process_t *c = root_proc(child);
    int rc;

    proc_pgrp_lock();
    if (parent == 0) {
        rc = pgrp_first_enter(child, child, ROOTTASK_PID);
    } else {
        root_process_t *pp = root_proc(parent);

        rc = pgrp_first_enter(child, root_process_pgrp(pp),
                              root_session_leader(pp));
        if (rc == EOK && pp->session) {
            c->session = pp->session;
            (void) atomic_add_value(&c->session->links, 1);
        }
    }
    proc_pgrp_unlock();
    return rc;
}

/* Take a process out of its group for good: reaped (the waiter collected it),
 * or its slot released. Idempotent, and a pid that no longer names the slot's
 * process leaves nothing. Callable with slot locks held.
 */
void proc_pgrp_reap(pid_t pid)
{
    root_process_t *p = root_proc(pid);

    pthread_mutex_lock(&pgtab_lock);
    if (p->pid == pid && p->pgrp_member) {
        pgrp_table_leave(&pgtab, p->pgrp, p->pgrp_gen);
        p->pgrp_member = 0;
    }
    pthread_mutex_unlock(&pgtab_lock);
}

/* Move the process whose slot lock the caller holds into group pgid of session
 * sid. The destination is entered before the old group is left, so a failed
 * move changes nothing, and a process reaped in the meantime is not moved.
 * Returns EOK, EPERM (no such group, or one in another session), or ESRCH.
 */
static int pgrp_move(root_process_t *p,
                     pid_t pgid,
                     pid_t sid,
                     enum pgrp_enter_mode mode)
{
    uint32_t gen;
    int rc;

    pthread_mutex_lock(&pgtab_lock);
    if (!p->pgrp_member) {
        rc = ESRCH;
    } else if ((rc = pgrp_table_enter(&pgtab, pgid, sid, mode, &gen)) == EOK) {
        pgrp_table_leave(&pgtab, p->pgrp, p->pgrp_gen);
        p->pgrp = pgid;
        p->pgrp_gen = gen;
    }
    pthread_mutex_unlock(&pgtab_lock);
    return rc;
}

/* setsid's new group: the process leads group pid, in session pid. The caller
 * holds pgrp_mutex and the process's slot lock. EPERM when a group of that id
 * exists, which is what POSIX asks for a process whose pid names one.
 */
int proc_pgrp_new(pid_t pid)
{
    return pgrp_move(root_proc(pid), pid, pid, PGRP_CREATE);
}

/* The checks and the change of one setpgid, with pgrp_mutex held. At most one
 * process slot is locked at a time, so the lock order of the rest of proc
 * (child, then parent) is never in play; nothing else can move a process
 * between groups or sessions while pgrp_mutex is held, so the sessions and the
 * group read here are the ones the change applies to. The group's own end is
 * the one thing that can still happen, and the change checks it itself.
 */
static int setpgid_locked(pid_t caller, pid_t target, pid_t pgid)
{
    struct proc_snap self, tgt;
    int rc;

    if (proc_snapshot(caller, &self) != EOK)
        return EL2HLT;
    if (target == caller) {
        tgt = self;
    } else {
        /* Only the caller's own children. */
        if (proc_snapshot(target, &tgt) != EOK ||
            (tgt.flags & RTMUX_FLG_PROC_WAITDONE) || tgt.ppid != caller)
            return ESRCH;

        /* A child that has replaced its image by exec is no longer the parent's
         * to move. A spawned child that has not is: RTMux spawns rather than
         * forks, so it is movable until it execs or exits.
         */
        if (tgt.execed)
            return EACCES;
    }

    /* A session leader may not change group, and the target must be in the
     * caller's session. A process with no session is in the system session, as
     * for getsid.
     */
    if ((tgt.flags & RTMUX_FLG_PROC_SLEADER) || tgt.sid != self.sid)
        return EPERM;

    /* pgid 0 means the target's own pid: a group of its own. Any other group
     * must have a member in the caller's session, whether or not the process it
     * is named for is still there.
     */
    if (pgid == 0)
        pgid = target;
    if (pgid == tgt.pgrp)
        return EOK;

    proc_pgrp_window(PGRP_WINDOW_SETPGID);

    if (!root_proc_lock(target))
        return ESRCH;
    rc = pgrp_move(root_proc(target), pgid, self.sid,
                   pgid == target ? PGRP_JOIN_OR_CREATE : PGRP_JOIN);
    root_proc_unlock(target);
    return rc;
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
