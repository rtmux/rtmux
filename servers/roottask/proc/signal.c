/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * proc_signal — deliver a signal as a notification to a target process.
 *
 * In RTMux, signals are not handled by the kernel. Instead, kill() sends a
 * _PROC_KILL message to roottask, which delivers a _NOTIFICATION_CODE_SIGNAL
 * notification to the target process's system channel. The target's system
 * thread receives the notification and invokes the user-registered handler.
 *
 * License: Apache 2.0
 */

#include "externs.h"
#include "root-process.h"
#include <private/root-proc-info.h>
#include "private.h"

/* Snapshot of the calling process used for every permission decision. */
struct signal_sender {
    pid_t pid;
    pid_t pgrp;
    pid_t sid;
    struct _cred_info cred;
};

/* proc_may_signal — POSIX kill() permission rule.
 *
 * Allowed when the sender is privileged, when its real or effective uid matches
 * the target's real or saved uid, or for SIGCONT within one session. Signal 0
 * is checked the same way.
 */
static int proc_may_signal(const struct _cred_info *s,
                           const struct _cred_info *t,
                           int signo,
                           pid_t s_sid,
                           pid_t t_sid)
{
    if (s->euid == 0)
        return 1;
    if (s->ruid == t->ruid || s->ruid == t->suid || s->euid == t->ruid ||
        s->euid == t->suid)
        return 1;
    return signo == SIGCONT && s_sid != 0 && s_sid == t_sid;
}

/* The bits of a kept set that name a signal: 1 .. NSIG-1. */
#define SIGNAL_KEPT_MASK (((1ULL << NSIG) - 1) & ~1ULL)

/* signal_post_locked — deliver signo to process pid, or keep it for later.
 *
 * A signal is a notification on the target's system channel, which its libc
 * registers early in crt0. A process can be named by kill from the moment its
 * spawn returns, which is before that, and an exec leaves it without one from
 * the end of the old image to the start of the new, so a signal for a live
 * process with no channel is kept in its slot and proc_syschan delivers it
 * when the channel appears. Like libc's own pending set, the kept set holds one
 * instance of a signal, and one sender, the last, for all of them. A process
 * that never registers a channel (it is not linked with this libc, or its
 * start-up failed) never gets them; a SIGKILL is kept only while the process
 * is loading (signal_one ends the process itself otherwise, and signal_loaded
 * does when loading is over).
 *
 * The kernel finds the channel from the pid at the moment of delivery and says
 * when there is none; no channel pointer is held here, as a process may destroy
 * its channel whenever it likes. Called with root_proc_lock(pid) held, which
 * orders the post against the registration (proc_syschan) and against exec's
 * removal of the old channel (signal_exec_keep_locked). Parameters:
 *   pid    : the target
 *   signo  : the signal, 1 .. NSIG-1
 *   sender : pid reported to the handler
 */
void signal_post_locked(pid_t pid, int signo, pid_t sender)
{
    /* Only "no channel" (ENXIO) is worth keeping: a signal that failed for
     * another reason (no notification soul, a channel being destroyed) would
     * otherwise sit in the kept set until some later image registers a channel
     * and be delivered to it, long after the fact.
     */
    if (signal_kill(pid, signo, sender) == ENXIO) {
        root_proc(pid)->early_sigs |= 1ULL << signo;
        root_proc(pid)->early_sender = sender;
    }
}

/* Carry out a kept SIGKILL, if there is one: the process is ended, not sent a
 * notification, and its other kept signals no longer matter.
 *
 * Returns 1 if this started the process's exit: the caller runs
 * proc_exit_finish after dropping the lock. Called with root_proc_lock(pid)
 * held.
 */
static int signal_take_kept_kill_locked(pid_t pid)
{
    root_process_t *slot = root_proc(pid);

    if (!(slot->early_sigs & (1ULL << SIGKILL)))
        return 0;
    slot->early_sigs = 0;
    return proc_exit_begin_locked(pid, SIGKILL, 1);
}

/* signal_flush_early_locked — deliver what signal_post_locked kept.
 *
 * Called by proc_syschan once the system channel is registered, with
 * root_proc_lock(pid) held across the registration and this call, so no signal
 * is kept after the flush or delivered ahead of an earlier one. A kept SIGKILL
 * (the process registered before signal_loaded got to it) is not delivered but
 * carried out, as everywhere else. A signal the kernel has no channel for stays
 * kept, with those after it; one that fails otherwise is dropped, as a post
 * that fails so would drop it. Parameters:
 *   pid : the process
 * Return value: 1 if a kept SIGKILL started the process's exit, and the caller
 * must run proc_exit_finish(pid) after dropping the lock; else 0.
 */
int signal_flush_early_locked(pid_t pid)
{
    root_process_t *slot = root_proc(pid);

    if (slot->early_sigs & (1ULL << SIGKILL))
        return signal_take_kept_kill_locked(pid);
    for (int signo = 1; signo < NSIG && slot->early_sigs; signo++) {
        if (!(slot->early_sigs & (1ULL << signo)))
            continue;
        if (signal_kill(pid, signo, slot->early_sender) == ENXIO)
            break;
        slot->early_sigs &= ~(1ULL << signo);
    }
    return 0;
}

/* signal_exec_keep_locked — move the old image's undelivered signals into the
 * kept set, for the new image.
 *
 * exec calls this with root_proc_lock(pid) held, after the old image's threads
 * are gone and before its channels are destroyed. Two sets are carried: the
 * signals still queued on the old system channel, which its system thread never
 * received, and the ones the old image's libc held pending when it asked for
 * the exec (blocked, or received while the exec was being prepared). The
 * process has no system channel from here until the new image registers one,
 * so every later signal is kept as well, and proc_syschan delivers them all, to
 * a libc that starts with the old image's blocked and ignored sets. Parameters:
 *   pid     : the process that execs
 *   pending : libc's pending set as a sigset_t word, signal n at bit n-1
 */
void signal_exec_keep_locked(pid_t pid, uint64_t pending)
{
    root_process_t *slot = root_proc(pid);
    pid_t sender = 0;
    uint64_t queued = process_syschan_detach(pid, &sender);

    /* libc does not say who sent what it held; a sender is reported only for
     * the queued ones, or stays as it was for signals kept earlier.
     */
    if (queued)
        slot->early_sender = sender;
    else if (!slot->early_sigs)
        slot->early_sender = 0;
    slot->early_sigs |= (queued | (pending << 1)) & SIGNAL_KEPT_MASK;
}

/* signal_loaded — the loader has started process pid: carry out a SIGKILL that
 * arrived while it was loading.
 *
 * A SIGKILL ends its target without asking it (signal_one), except while the
 * target's one thread is the loader, which runs roottask's code with roottask's
 * locks; then it is kept like any early signal. This is called by the spawn
 * handler right after the start commit and does not wait for the process to
 * register a system channel, which one without this libc never does. The lock
 * is taken even when nothing looks kept: signal_one decides under it, so a look
 * without it could miss a SIGKILL being kept just now.
 */
void signal_loaded(pid_t pid)
{
    process_t *prp;
    int killed;

    if (!(prp = root_proc_lock(pid)))
        return;
    killed = signal_take_kept_kill_locked(pid);
    root_proc_unlock(pid);
    if (killed)
        proc_exit_finish(pid);
}

/* Which processes a kill() pid argument selects. */
enum kill_scope { KILL_ONE, KILL_PGRP, KILL_ALL };

/* signal_one — deliver (or permission-check) a signal to one process.
 *
 * Re-validates under the target's lock that the slot still holds a live process
 * that matches the requested scope.
 *
 * Returns ESRCH when it does not match (the caller does not count it), EPERM
 * when it matches but the sender may not signal it, EOK on delivery, or another
 * errno on a delivery failure.
 */
static int signal_one(const struct signal_sender *snd,
                      pid_t target_pid,
                      enum kill_scope scope,
                      pid_t pgrp,
                      int signo)
{
    root_proc_info_t qinfo;
    struct _cred_info tcred;
    process_t *prp;
    int rc, killed = 0;

    if (!(prp = root_proc_lock(target_pid)))
        return ESRCH;

    /* Only the existence probe treats a dying or zombie target as gone; real
     * signals are still delivered to it, as before. A target with no system
     * channel is one that has not registered it yet (it is signalled through
     * signal_post_locked) unless it is on its way out, when the channel is gone
     * for good.
     */
    if (process_query_info(target_pid, &qinfo) != EOK ||
        ((qinfo.flags & PROC_DEAD_FLAGS) && (signo == 0 || !qinfo.sys_chp))) {
        rc = ESRCH;
    } else if ((scope == KILL_PGRP && qinfo.pgrp != pgrp) ||
               (scope == KILL_ALL &&
                (target_pid == ROOTTASK_PID || target_pid == snd->pid ||
                 root_proc(target_pid)->system))) {
        rc = ESRCH;
    } else if ((rc = cred_get_call(target_pid, &tcred)) == EOK) {
        if (!proc_may_signal(&snd->cred, &tcred, signo, snd->sid,
                             qinfo.sess_leader)) {
            rc = EPERM;
        } else if (signo == SIGKILL && target_pid != ROOTTASK_PID &&
                   !(qinfo.flags & RTMUX_FLG_PROC_LOADING)) {
            /* SIGKILL is not delivered: roottask ends the process itself, at
             * once, because it may not depend on what the target's libc is
             * doing or whether it has one. A process still loading is the
             * exception (its one thread is roottask's loader): that branch is
             * skipped and the SIGKILL is kept for signal_loaded.
             */
            if (!(qinfo.flags & PROC_DEAD_FLAGS))
                killed =
                    proc_exit_begin_locked(target_pid, SIGKILL, 1);
        } else if (signo != 0) {
            signal_post_locked(target_pid, signo, snd->pid);
        }
    }

    root_proc_unlock(target_pid);
    if (killed)
        proc_exit_finish(target_pid);
    return rc;
}

/* Snapshot the caller's pgrp, session, and credentials once. */
static int signal_sender_snapshot(pid_t pid, struct signal_sender *snd)
{
    root_proc_info_t qinfo;
    int rc;

    if (!root_proc_lock(pid))
        return ESRCH;
    if ((rc = process_query_info(pid, &qinfo)) == EOK)
        rc = cred_get_call(pid, &snd->cred);
    root_proc_unlock(pid);
    if (rc != EOK)
        return rc;
    snd->pid = pid;
    snd->pgrp = qinfo.pgrp;
    snd->sid = qinfo.sess_leader;
    return EOK;
}

/* proc_signal — handle _PROC_KILL message.
 *
 * @ctp : I/O service context (ctp->info.pid = calling process)
 * @msg : message with target pid and signal number
 *
 * pid > 0 selects one process; 0 the caller's process group; < -1 the group
 * -pid; -1 every process the sender may signal except pid 1, the sender, and
 * system servers. Result: EOK if any delivery succeeded; else the first
 * delivery error; else EPERM if targets matched but none was permitted; else
 * ESRCH.
 *
 * Return EOK on success, errno on failure
 */
int proc_signal(ioserv_context_t *ctp, proc_kill_t *msg)
{
    struct signal_sender snd;
    pid_t target_pid = msg->i.pid;
    int signo = msg->i.signo;
    enum kill_scope scope = KILL_ONE;
    pid_t pgrp = 0;
    int rc, delivered = 0, denied = 0, err = EOK;

    if (signo < 0 || signo >= NSIG || target_pid == INT_MIN)
        return EINVAL;

    if ((rc = signal_sender_snapshot(ctp->info.pid, &snd)) != EOK)
        return rc;

    if (target_pid > 0) {
        return signal_one(&snd, target_pid, KILL_ONE, 0, signo);
    }
    if (target_pid == -1) {
        scope = KILL_ALL;
    } else {
        scope = KILL_PGRP;
        pgrp = target_pid == 0 ? snd.pgrp : -target_pid;
    }

    for (unsigned i = 0; i < ROOT_PROCESS_MAX; i++) {
        pid_t pid = root_process[i].pid;

        if (pid == 0)
            continue;
        rc = signal_one(&snd, pid, scope, pgrp, signo);
        if (rc == EOK)
            delivered = 1;
        else if (rc == EPERM)
            denied = 1;
        else if (rc != ESRCH && err == EOK)
            err = rc;
    }
    if (delivered)
        return EOK;
    if (err != EOK)
        return err;
    return denied ? EPERM : ESRCH;
}
