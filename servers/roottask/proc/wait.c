/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/wait.h>
#include <sys/wait-ops.h>
#include <private/root-proc-info.h>
#include "externs.h"
#include "private.h"
#include "root-process.h"

#define ROOT_WAIT_OPT_MASK \
    (WEXITED | WUNTRACED | WCONTINUED | WNOHANG | WNOWAIT | RTMUX_WTRAPPED)
#define WAIT_CHILD_PID_COUNT 256

soul_t wait_souls INITSOUL(-1, struct wait_entry, 2, 8, ~0);

/* wait_unblock — io-unblock handler for a thread blocked in wait().
 *
 * Removes the matching wait_entry from the process's wait list and replies
 * EINTR to the blocked thread. Also called without msg from wait_close_ocb() to
 * tear down on close.
 *
 * Parameters:
 *   ctp : I/O service context for the unblock
 *   msg : the unblock notification, or NULL when called on close
 *   ocb : open control block (unused)
 * Return value: IOSERV_NOREPLY.
 */
static int wait_unblock(ioserv_context_t *ctp,
                        io_notification_t *msg,
                        void *ocb)
{
    process_t *prp;
    struct _msg_info info;
    pid_t pid;
    root_proc_info_t qinfo;

    pid = ctp->info.pid;

    if ((prp = root_proc_lock(pid))) {
        /* msg will be NULL if we're called from wait_close_ocb() */
        if (msg && ((msg_info(ctp->rcvid, &info) == -1) ||
                    !(info.flags & RTMUX_MI_UNBLOCK_REQ))) {
            root_proc_unlock(pid);
            return IOSERV_NOREPLY;
        }

        /* prp lives in kernel heap (PTE_U=0) — fetch flags via the
         * process_query_info snapshot rather than dereferencing here.
         */
        if (process_query_info(pid, &qinfo) == EOK &&
            (qinfo.flags & (RTMUX_FLG_PROC_LOADING | RTMUX_FLG_PROC_TERMING)) ==
                0) {
            struct wait_entry *p, **pp;
            root_process_t *tmp = root_proc(pid);

            for (pp = &tmp->wap; (p = *pp); pp = &p->next) {
                if (p->rcvid == ctp->rcvid) {
                    *pp = p->next;

                    if (!tmp->wap) {
                        (void) ioserv_unbind(&ctp->info);
                    }
                    msg_error(ctp->rcvid, EINTR);
                    proc_object_free(&wait_souls, p);
                    break;
                }
            }
        }
        root_proc_unlock(pid);
    }
    return IOSERV_NOREPLY;
}

static const ioserv_ops_t proc_wait_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .unblock = wait_unblock,
};

/* Return -1
 *     This prp can never match the criteria
 * Return 0
 *     This prp does not currently match the criteria
 * Return WNOHANG
 *     An error with wap, reply was done (i.e. a thread unblocked)
 * Return non-zero W?????
 *     This prp matched and a reply was done (i.e. a thread unblocked)
 */
/* proc_wait_check — test one child against a waiter's criteria and, if it
 * matches, unblock the waiter with the child's exit status.
 *
 * Takes pid_t (not process_t*) so the caller never dereferences the
 * kernel-allocated process_t (heap PTE_U=0). All reads come from the
 * process_query_info snapshot; the terminal writes (clear WAITINFO, set
 * WAITDONE, fold cpu times, clear si_signo) happen kernel-side via
 * ProcessWaitFinalize.
 *
 * See the return-value legend in the comment block above.
 */
int proc_wait_check(pid_t prp_pid,
                    pid_t parent_pid,
                    struct wait_entry *wap,
                    int match)
{
    struct _msg_info info;
    root_proc_info_t qinfo;
    int send_term_notification = 0;
    unsigned terming_priority = 0;
    int ret_match;

    if (process_query_info(prp_pid, &qinfo) != EOK) {
        return -1;
    }

    /* Check if this prp matched the requested group */
    switch (wap->idtype) {
    case P_ALL:
        break;

    case P_PID:
        if (qinfo.pid != wap->id) {
            return -1;
        }
        break;

    case P_PGID:
        if (qinfo.pgrp != wap->id) {
            return -1;
        }
        break;

    default:
        msg_error(wap->rcvid, EINVAL);
        return WNOHANG;
    }

    if (match) {
        /* Check for match, if parent is ignoring SIGCHLD, are we the last
         * child?
         */
        match &= wap->options;
        /* Have we already responded with exit code? */
        if (!(qinfo.flags & RTMUX_FLG_PROC_WAITINFO)) {
            match &= ~WEXITED;
        }
    } else {
        /* Check if the requested prp currently matches any options */
        if ((wap->options & RTMUX_WTRAPPED) &&
            (qinfo.flags &
             (RTMUX_FLG_PROC_DEBUG_STOPPED | RTMUX_FLG_PROC_PTRACED)) ==
                (RTMUX_FLG_PROC_DEBUG_STOPPED | RTMUX_FLG_PROC_PTRACED)) {
            match = RTMUX_WTRAPPED;
        } else if ((wap->options & WEXITED) &&
                   (qinfo.flags & RTMUX_FLG_PROC_WAITINFO)) {
            match = WEXITED;
        } else if ((wap->options & WCONTINUED) &&
                   (qinfo.flags & RTMUX_FLG_PROC_CONTINUED)) {
            match = WCONTINUED;
        } else if ((wap->options & WUNTRACED) &&
                   (qinfo.flags & RTMUX_FLG_PROC_STOPPED) &&
                   qinfo.siginfo.si_signo != 0) {
            match = WUNTRACED;
        }
    }

    /* If no options matched, check if it could ever match options */
    if (match == 0) {
        int options = wap->options;

        if (qinfo.flags & (RTMUX_FLG_PROC_ZOMBIE | RTMUX_FLG_PROC_TERMING)) {
            options &= ~(WUNTRACED | RTMUX_WTRAPPED | WCONTINUED);
        }
        if ((qinfo.flags & (RTMUX_FLG_PROC_ZOMBIE | RTMUX_FLG_PROC_WAITINFO)) ==
            RTMUX_FLG_PROC_ZOMBIE) {
            options &= ~WEXITED;
        }
        if (qinfo.flags & RTMUX_FLG_PROC_NOZOMBIE) {
            options &= ~WEXITED;
        }
        if (qinfo.flags & RTMUX_FLG_PROC_WAITDONE) {
            options &= ~WEXITED;
        }
        if (!(qinfo.flags & RTMUX_FLG_PROC_PTRACED)) {
            options &= ~RTMUX_WTRAPPED;
        }
        if ((options & (WEXITED | WUNTRACED | RTMUX_WTRAPPED | WCONTINUED)) ==
            0) {
            return -1;
        }
        return 0;
    }

    /* Unblock the waiting thread... */

    if (msg_info(wap->rcvid, &info) != -1) {
        siginfo_t siginfo;
        root_process_t *parent_tmp = root_proc(parent_pid);

        /* unbind and unblock if rcvid is still around */
        if ((!parent_tmp->wap) ||
            ((parent_tmp->wap == wap) && !parent_tmp->wap->next)) {
            (void) ioserv_unbind(&info);
        }
        siginfo = qinfo.siginfo;
        if (siginfo.si_signo != SIGCHLD) {
            if (qinfo.flags & RTMUX_FLG_PROC_COREDUMP) {
                siginfo.si_code = CLD_DUMPED;
            } else {
                siginfo.si_code = CLD_KILLED;
            }
            siginfo.si_status = siginfo.si_signo;
            siginfo.si_pid = qinfo.pid;
            siginfo.si_signo = SIGCHLD;
        }
        if (info.flags & RTMUX_MI_ENDIAN_DIFF) {
            ENDIAN_SWAP32(&siginfo.si_signo);
            ENDIAN_SWAP32(&siginfo.si_code);
            ENDIAN_SWAP32(&siginfo.si_errno);
            ENDIAN_SWAP32(&siginfo.si_pid);
            ENDIAN_SWAP32(&siginfo.si_status);
            ENDIAN_SWAP32(&siginfo.si_utime);
            ENDIAN_SWAP32(&siginfo.si_stime);
        }
        msg_reply(wap->rcvid, 0, &siginfo, sizeof siginfo);
    } else {
        root_warn("proc_wait_check: msg_info() failed, errno=%d\n", errno);
    }

    /* Clean up prp status if requested so it is not reported again */
    if (wap->options & WNOWAIT) {
        return WNOWAIT;
    }

    /* A stop or continue is reported once. Its siginfo must not outlive the
     * report, or the next waiter would see the same event and a later fault
     * would be masked by it.
     */
    if (match == WUNTRACED || match == WCONTINUED) {
        root_proc(prp_pid)->siginfo.si_signo = 0;
    }
    if (ProcessWaitFinalize(prp_pid, parent_pid, match, &ret_match,
                            &send_term_notification,
                            &terming_priority) != EOK) {
        return match; /* prp gone — caller still treats as matched */
    }
    if (match == WEXITED)
        procfs_wait_stale(prp_pid);
    if (send_term_notification) {
        msg_send_notification(ROOTTASK_COID, (int) terming_priority,
                              PROC_CODE_TERM, prp_pid);
    }
    return ret_match;
}

/* proc_wait — handle a _PROC_WAIT message (waitpid / waitid).
 *
 * Enumerates the caller's children and checks each against the wait criteria.
 * If a child already matches, replies immediately; otherwise queues a
 * wait_entry on the process and blocks the caller (unless WNOHANG was
 * requested).
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = the waiting process)
 *   msg : the wait request (idtype / id / options)
 * Return value: an errno, or a IOSERV_* status code.
 */
int proc_wait(ioserv_context_t *ctp, proc_wait_t *msg)
{
    process_t *prp;
    pid_t locked_pid = 0;
    struct wait_entry *wap, **pwap, waitl;
    int alive;

    /* Snapshot of children pid_t list — kernel-side enumeration via
     * ProcessListChildren replaces the old prp->child / sibling walk which
     * dereferenced the kernel-allocated process_t directly.
     */
    pid_t child_pids[WAIT_CHILD_PID_COUNT];
    size_t child_count = 0;
    size_t i_child;

    root_trace("proc_wait: ENTER pid=%d idtype=%d id=%ld options=%x\n",
               ctp->info.pid, msg->i.idtype, (long) msg->i.id, msg->i.options);

    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
        ENDIAN_SWAP16(&msg->i.idtype);
        ENDIAN_SWAP32(&msg->i.options);
        ENDIAN_SWAP32(&msg->i.id);
    }
    if (msg->i.options & ~ROOT_WAIT_OPT_MASK) {
        return EINVAL;
    }
    waitl.rcvid = ctp->rcvid;
    waitl.idtype = msg->i.idtype;
    waitl.options = (short) msg->i.options;
    waitl.id = msg->i.id;

    alive = 0;
    locked_pid = ctp->info.pid;
    if (!(prp = root_proc_lock(locked_pid))) {
        return EL2HLT;
    }

    if (ProcessListChildren(locked_pid, child_pids,
                            sizeof child_pids / sizeof child_pids[0],
                            &child_count) != EOK) {
        return proc_error(ESRCH, locked_pid);
    }
    for (i_child = 0; i_child < child_count; i_child++) {
        root_trace("proc_wait: pid=%d checking child=%d\n", ctp->info.pid,
                   child_pids[i_child]);
        switch (proc_wait_check(child_pids[i_child], locked_pid, &waitl, 0)) {
        case 0:
            alive++;
            break;
        case -1:
            break;
        default:
            return proc_error(IOSERV_NOREPLY, locked_pid);
        }
    }

    if (alive == 0) {
        return proc_error(ECHILD, locked_pid);
    }

    if (waitl.options & WNOHANG) {
        memset(&msg->o, 0x00, sizeof msg->o);
        return proc_error(IOSERV_PTR(ctp, &msg->o, sizeof msg->o), locked_pid);
    }

    /* nothing waiting, so add to queue sorted so pid match has higher priority
     */
    if (!(wap = proc_object_alloc(&wait_souls))) {
        return proc_error(ENOMEM, locked_pid);
    }
    for (pwap = &root_proc(locked_pid)->wap; (waitl.next = *pwap);
         pwap = &waitl.next->next) {
        if (waitl.next->idtype < waitl.idtype) {
            break;
        }
    }
    *wap = waitl;
    *pwap = wap;

    ctp->id = root_id;
    (void) ioserv_open_bind(ctp, wap, &proc_wait_funcs);
    return proc_error(IOSERV_NOREPLY, locked_pid);
}

/* proc_stop_or_continue — PROC_CODE_STOP / PROC_CODE_CONT notification handler.
 *
 * When a process stops or continues, wakes any of its parent's waiters that
 * requested WSTOPPED / WCONTINUED notification.
 *
 * Parameters:
 *   ctp    : message context (notification value = affected pid)
 *   code   : PROC_CODE_STOP or PROC_CODE_CONT
 *   flags  : dispatch flags (unused)
 *   handle : dispatch handle (unused)
 * Return value: 0.
 */
int proc_stop_or_continue(message_context_t *ctp,
                          int code,
                          unsigned flags,
                          void *handle)
{
    union sigval value = ctp->msg->notification.value;
    process_t *prp;
    process_t *parent;
    int type;
    int status;
    struct wait_entry *wap, **pwap;

    if (proc_service_scoid != ctp->msg->notification.scoid) {
        return 0;
    }

    if ((prp = root_proc_lock(value.sival_int))) {
        pid_t prp_pid = value.sival_int;
        pid_t parent_pid = root_proc(prp_pid)->parent_pid;
        if ((parent = root_proc_lock(parent_pid))) {
            type = (code == PROC_CODE_CONT) ? WCONTINUED : WSTOPPED;
            for (pwap = &root_proc(parent_pid)->wap; (wap = *pwap);) {
                if ((status = proc_wait_check(prp_pid, parent_pid, wap, type)) >
                    0) {
                    *pwap = wap->next;
                    proc_object_free(&wait_souls, wap);
                    if (status == type) {
                        break;
                    }
                } else {
                    pwap = &wap->next;
                }
            }
            root_proc_unlock(parent_pid);
        }
        root_proc_unlock(prp_pid);
    }
    return 0;
}

/* proc_nozombie — detach a child from its parent's wait queue, delivering its
 * exit status, and mark it NOZOMBIE so no zombie lingers. Parameters:
 *   pid    : pid of the exiting child
 *   status : exit status to report to a waiter
 */
void proc_nozombie(pid_t pid, int status)
{
    /* This should use CLD_EXITED instead of SI_USER, but the signal_kill()
     * kernel call does not allow it. SI_USER is allowable though, and this is
     * only used by process-detach paths right now. Process state lives in
     * kernel heap (PTE_U=0); writes go through a kernel extension. Use
     * process_query_info to gate on NOZOMBIE, then stamp siginfo and set
     * WAITINFO|NOZOMBIE via ProcessNozombieMark. This path returns early when
     * NOZOMBIE was already set.
     */
    {
        root_proc_info_t qinfo;
        pid_t parent_pid;
        process_t *parent;
        struct wait_entry *wap, **pwap;

        if (process_query_info(pid, &qinfo) != EOK)
            return;
        if (qinfo.flags & RTMUX_FLG_PROC_NOZOMBIE)
            return;

        if (ProcessNozombieMark(pid, SIGCHLD, CLD_EXITED, pid, status) != EOK)
            return;

        parent = root_proc_lock_parent(pid, &parent_pid);

        for (pwap = &root_proc(parent_pid)->wap; (wap = *pwap);) {
            if ((status = proc_wait_check(pid, parent_pid, wap, WEXITED)) > 0) {
                *pwap = wap->next;
                proc_object_free(&wait_souls, wap);
                if (status == WEXITED) {
                    break;
                }
            } else {
                pwap = &wap->next;
            }
        }
        root_proc_unlock(parent_pid);
        (void) parent;

        /* SIGCHLD replaced by notification — delivered by kext_process_shutdown
         */
    }
}
