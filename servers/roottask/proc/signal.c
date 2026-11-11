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

/* chp is an opaque kernel-VA pointer (the target's prp->sys_chp); roottask
 * never dereferences it, just passes it through to the kext.
 */

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
    static const unsigned dead =
        RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_ZOMBIE |
        RTMUX_FLG_PROC_WAITINFO | RTMUX_FLG_PROC_WAITDONE |
        RTMUX_FLG_PROC_DESTROYALL;
    root_proc_info_t qinfo;
    struct _cred_info tcred;
    int rc;

    if (!root_proc_lock(target_pid))
        return ESRCH;

    /* Only the existence probe treats a dying or zombie target as gone; real
     * signals are still delivered to it, as before.
     */
    if (process_query_info(target_pid, &qinfo) != EOK || !qinfo.sys_chp ||
        (signo == 0 && (qinfo.flags & dead))) {
        rc = ESRCH;
    } else if ((scope == KILL_PGRP &&
                (qinfo.pgrp ? qinfo.pgrp : target_pid) != pgrp) ||
               (scope == KILL_ALL &&
                (target_pid == ROOTTASK_PID || target_pid == snd->pid ||
                 root_proc(target_pid)->system))) {
        rc = ESRCH;
    } else if ((rc = cred_get_call(target_pid, &tcred)) == EOK) {
        if (!proc_may_signal(&snd->cred, &tcred, signo, snd->sid,
                             qinfo.sess_leader)) {
            rc = EPERM;
        } else if (signo != 0) {
            signal_kill(qinfo.sys_chp, signo, snd->pid);
        }
    }

    root_proc_unlock(target_pid);
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
    /* A process that never joined a group is its own group (pgrp 0 = unset). */
    snd->pgrp = qinfo.pgrp ? qinfo.pgrp : pid;
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
