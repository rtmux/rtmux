/*
 * exit.c — handle _PROC_EXIT message from a dying process.
 *
 * Called by the proc service dispatch loop when a process sends _PROC_EXIT
 * (from _exit()). The pool thread performs all cleanup directly — no terminator
 * thread is created in the dying process.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <sys/procmsg.h>
#include <private/root-proc-info.h>
#include "private.h"
#include "root-process.h"
#include "as.h"

/* proc_exit_begin_locked — start the exit of process pid, once.
 *
 * Records the status wait() will report, marks the process terminating and
 * steers every one of its threads to self-reap; the last thread out raises the
 * notification on which proc_terminate frees the process's resources. The
 * status goes into root_process_t.siginfo because kext_proc_exit_mark ignores
 * the value it is given and proc_terminate would otherwise report 0. It needs
 * nothing from the process itself, so it serves a process that asked to exit
 * (proc_exit), one roottask ends without asking it (SIGKILL, signal.c), one
 * whose exec failed, and exec's loader shell. The first caller decides the
 * status: a process already on its way out is left as it is, so an _exit that
 * was queued behind a SIGKILL, or the reverse, cannot change what wait()
 * reports. Called with root_proc_lock(pid) held.
 *
 * Only the marking happens here. Destroying the threads can take a while: a
 * pass is made again for as long as a thread is held by a message transfer or
 * is on its way into a stop. That is proc_exit_finish's job, which the caller
 * runs after it has dropped the lock, so that other requests about the process
 * are not kept waiting meanwhile. Parameters:
 *   pid       : the process
 *   status    : the exit code, or the signal number when by_signal
 *   by_signal : nonzero if the process is ended by a signal
 * Return value: 1 if this call started the exit, and the caller must run
 * proc_exit_finish(pid); 0 if the exit had started already.
 */
int proc_exit_begin_locked(pid_t pid, int status, int by_signal)
{
    root_process_t *tmp = root_proc(pid);

    if (tmp->exit_begun)
        return 0;
    tmp->exit_begun = 1;
    tmp->siginfo.si_signo = SIGCHLD;
    tmp->siginfo.si_code = by_signal ? CLD_KILLED : CLD_EXITED;
    tmp->siginfo.si_status = status;
    tmp->siginfo.si_pid = pid;
    (void) ProcessExitMark(pid, status);
    return 1;
}

/* proc_exit_finish — destroy the threads of a process whose exit
 * proc_exit_begin_locked started, without holding its lock.
 *
 * Each pass takes the process lock, which keeps the kernel's process alive for
 * the call, and drops it before the 1 ms pause that a pass asking to be made
 * again is followed by. The passes end when every thread is gone or the process
 * is: nothing outside the process holds a thread up for longer than one message
 * transfer. Parameters:
 *   pid : the process
 */
void proc_exit_finish(pid_t pid)
{
    static const struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};

    for (unsigned tries = 1;; tries++) {
        process_t *prp = root_proc_lock(pid);
        int rc;

        if (!prp)
            return;
        rc = process_threads_destroy(prp);
        root_proc_unlock(pid);
        if (rc != EAGAIN)
            return;
        if (tries % 1000 == 0)
            root_warn("exit: pid %d: threads still being destroyed after %u "
                      "passes\n",
                      (int) pid, tries);
        (void) nanosleep(&ts, NULL);
    }
}

/* proc_exit — handle a _PROC_EXIT message from a dying process.
 *
 * Runs the full teardown sequence directly on the pool thread: mark
 * terminating, destroy threads / events / channels / timers / FDs, release conf
 * vars and resources, unmap memory and shut down the address space, detach
 * namespace nodes, then wake a matching waitpid. Reaps the process if a waiter
 * collected it, else leaves a minimal zombie.
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = dying process)
 *   msg : the exit message (carries the exit status)
 * Return value: IOSERV_NOREPLY.
 */
int proc_exit(ioserv_context_t *ctp, union proc_msg_union *msg)
{
    struct _proc_exit *ep = (struct _proc_exit *) (void *) &msg->type;
    pid_t pid = ctp->info.pid;
    process_t *prp;
    int started;

    if (ctp->info.msglen < (int) sizeof(*ep) || ep->by_signal > 1 ||
        (ep->by_signal && (ep->status < 1 || ep->status > MAXSIG)))
        return EINVAL;

    prp = root_proc_lock(pid);
    if (!prp) {
        root_log("proc_exit: pid %d not found\n", pid);
        msg_error(ctp->rcvid, ESRCH);
        return IOSERV_NOREPLY;
    }

    root_trace("proc_exit: pid=%d status=%d\n", pid, ep->status);

    /* rc12 — unified teardown. proc_exit no longer frees ANY resources here. It
     * records the exit status, marks the process terminating, and steers every
     * thread to self-reap. The LAST thread out fires the proc_threads_destroyed
     * notification, and the single resource-cleanup path in proc_terminate then
     * frees events / channels / timers / FDs / conf / hwres / memory / soul and
     * notifies the parent.
     *
     * This deletes the second, duplicated copy of that whole sequence
     * (proc_terminate path-2 was identical) AND the race it caused: the old
     * in-line cleanup freed channels and the address space while threads were
     * still reaping, papered over by the IPI_TLB_SAFE spin-drain in
     * process_shutdown. Now resources are freed strictly after every thread is
     * gone — by causality, not hopeful sequencing.
     */
    started = proc_exit_begin_locked(pid, ep->status, ep->by_signal);

    root_proc_unlock(pid);
    if (started)
        proc_exit_finish(pid);
    return IOSERV_NOREPLY;
}

/* proc_terminate — PROC_CODE_TERM notification handler.
 *
 * Two entry paths:
 *   1. Zombie reap: parent's waitpid() collected the exit status
 *      (ZOMBIE flag set by proc_exit).  Just call process_destroy_call.
 *   2. Abnormal death: proc_threads_destroyed fired because the
 *      last thread died without going through _exit() / proc_exit
 *      (e.g. ldso failure → DESTROYALL).  Do resource cleanup here
 *      (channels, timers, FDs, memory) then notify the parent.
 */
int proc_terminate(message_context_t *ctp,
                   int code,
                   unsigned flags,
                   void *handle)
{
    pid_t pid = ctp->msg->notification.value.sival_int;
    process_t *prp;
    int woke = 0;

    /* Accept PROC_CODE_TERM from two trusted sources:
     *   - roottask's own self-notification (proc_wait / spawn / fork retry)
     *     arrives on proc_service_scoid;
     *   - the kernel's last-thread proc_threads_destroyed notification
     *     (sigevent_notification_roottask) arrives on the system side channel
     *     ROOTTASK_COID.
     * A forged notification from an ordinary client carries neither scoid (its
     * connection's server scoid is an ordinary, non-side-channel id), so the
     * teardown trigger stays unspoofable.
     */
    if (ctp->msg->notification.scoid != proc_service_scoid &&
        ctp->msg->notification.scoid != (int) ROOTTASK_COID)
        return 0;

    if (!(prp = root_proc_lock(pid)))
        return 0;

    {
        root_proc_info_t qinfo_term;
        if (process_query_info(pid, &qinfo_term) != EOK) {
            root_proc_unlock(pid);
            return 0;
        }

        if (qinfo_term.flags & RTMUX_FLG_PROC_ZOMBIE) {
            /* Path 1: already cleaned up by proc_exit, just reap */
            root_proc_unlock(pid);
            process_destroy_call(pid);
            root_trace("proc_terminate: pid=%d reaped\n", pid);
            return 0;
        }

        if (qinfo_term.num_total_threads != 0) {
            /* Threads still alive — not ready for cleanup yet.
             * num_total_threads = threads.nentries - threads.nfree was the
             * original check (inverted: alive when != 0).
             */
            root_proc_unlock(pid);
            return 0;
        }

        /* Path 2 (rc12): the SOLE resource-cleanup path for both normal exit
         * and abnormal death. All threads are gone (checked above); do the full
         * cleanup. TERMING is EXPECTED here now — normal exit sets it in
         * proc_exit and then only steers threads to self-reap; the last thread
         * out delivered this notification.
         *
         * Skip ONLY a process that is purely mid-load and NOT dying: a
         * genuinely-loading process hasn't reaped its threads, so it would not
         * even reach here (no last-thread notification) — but guard anyway. A
         * process that died DURING load (LOADING set together with
         * TERMING/DESTROYALL) MUST be cleaned up here: its threads are gone
         * (ntot==0), and proc_terminate is now the only path that will do it
         * (the old synchronous proc_exit used to, ignoring LOADING). Cleanup
         * below also runs loader_exit, which replies to the deferred process
         * startup request.
         */
        if ((qinfo_term.flags & RTMUX_FLG_PROC_LOADING) &&
            !(qinfo_term.flags &
              (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL))) {
            root_proc_unlock(pid);
            return 0;
        }
    }

    root_trace("proc_terminate: pid=%d abnormal cleanup\n", pid);

    /* Stamp prp's kernel-side flags + siginfo (which now lives in
     * root_process_t). Use ProcessExitMark for the kernel half and write
     * root_proc(pid)->siginfo directly here.
     */
    {
        root_process_t *tmp = root_proc(pid);
        root_proc_info_t fault_info;

        /* A stop or continue nobody collected is not how the process died. */
        if (tmp->siginfo.si_signo == SIGCHLD &&
            (tmp->siginfo.si_code == CLD_STOPPED ||
             tmp->siginfo.si_code == CLD_CONTINUED)) {
            memset(&tmp->siginfo, 0, sizeof tmp->siginfo);
        }
        if (!tmp->siginfo.si_signo &&
            process_query_info(pid, &fault_info) == EOK &&
            fault_info.siginfo.si_signo != 0) {
            tmp->siginfo = fault_info.siginfo;
            tmp->siginfo.si_pid = pid;
            /* The kernel's record keeps the fault address where the status
             * goes; this copy is read as an exit record.
             */
            tmp->siginfo.si_status = tmp->siginfo.si_signo;
        }
        if (!tmp->siginfo.si_signo) {
            tmp->siginfo.si_signo = SIGCHLD;
            tmp->siginfo.si_code = CLD_EXITED;
            tmp->siginfo.si_status = 0;
            tmp->siginfo.si_pid = pid;
        }
        (void) ProcessExitMark(pid, tmp->siginfo.si_status);
    }

    /* Cleanup: channels, timers, FDs, connections */
    proc_event_destroy(pid);

    (void) ProcessDestroyChancons(pid, TYPE_CHANNEL);

    unsigned i = -1U;
    while (query_object(_QUERY_PROCESS, (unsigned) pid, _QUERY_PROCESS_TIMERS,
                        ++i, &i, 0, 0)) {
        process_timer_destroy(prp, (int) i);
    }

    (void) ProcessDestroyFdcons(pid);
    (void) ProcessDestroyChancons(pid, TYPE_CONNECTION);

    roottask_sysconf_destroy(pid);
    hwres_destroy_process(pid);

    /* Unmap memory. prp->memory / prp->flags / prp->pls all live in the gated
     * process_t; pull them via process_query_info and use the existing
     * ProcessClearPls kernel extension for the pls write.
     */
    root_proc_unlock(pid);
    (void) root_proc_lock(pid);
    {
        root_proc_info_t qinfo_unmap;

        if (process_query_info(pid, &qinfo_unmap) == EOK &&
            qinfo_unmap.has_memory) {
            (void) ProcessClearPls(pid);
            if (proc_wlock_adp(pid) != -1) {
                proc_lock_owner_mark(pid);
                root_vm_ops.munmap(pid, prp, 0, ~(size_t) 0, 0);
                as_mdestroy_pid(pid);
                proc_unlock_adp(pid);
            }
            proc_wlock_adp(ROOTTASK_PID);
            while (process_shutdown(prp, 0) == -1 && errno == EAGAIN)
                ;
            proc_unlock_adp(ROOTTASK_PID);
        }
    }
    root_proc_unlock(pid);

    while (process_shutdown(prp, 0) == -1 && errno == EAGAIN)
        ;

    /* Detach namespace nodes. A request the process sent before it died can
     * still be on its way through roottask, and reads root and cwd under the
     * process lock: clear them under it too, so that it finds both or neither.
     */
    {
        root_process_t *tmp = root_proc(pid);
        node_t *root = NULL, *cwd = NULL;

        /* A failed lock means the slot is no longer this process's. */
        if (root_proc_lock(pid)) {
            root = tmp->root;
            cwd = tmp->cwd;
            tmp->root = NULL;
            tmp->cwd = NULL;
            root_proc_unlock(pid);
        }
        if (root)
            path_node_detach(root);
        if (cwd)
            path_node_detach(cwd);
    }
    /* Notify parent */
    {
        pid_t parent_pid;
        root_process_t *parent_tmp;

        (void) root_proc_lock_parent(pid, &parent_pid);
        parent_tmp = root_proc(parent_pid);

        /* debug_name + TERMING/PRIV flags are kernel-side; lift via the
         * existing ProcessExitFinalize kernel extension.
         */
        (void) ProcessExitFinalize(pid);

        struct wait_entry **pwap, *wap;
        for (pwap = &parent_tmp->wap; (wap = *pwap);) {
            int status = proc_wait_check(pid, parent_pid, wap, WEXITED);
            if (status > 0) {
                *pwap = wap->next;
                proc_object_free(&wait_souls, wap);
                woke++;
                if (status == WEXITED)
                    break;
            } else {
                pwap = &wap->next;
            }
        }

        root_proc_unlock(parent_pid);
    }

    {
        root_proc_info_t qinfo_lcp2;
        if (process_query_info(pid, &qinfo_lcp2) == EOK && qinfo_lcp2.lcp) {
            /* loader_exit clears prp->lcp via process_clear_lcp. A child that
             * died while its spawn was still pending was reported to the parent
             * as an error, with no pid, so nobody can ever wait for it: reap it
             * now instead of leaving a zombie until the parent exits.
             */
            if (loader_exit(NULL, NULL, pid))
                woke++;
        }
    }

    /* A child detached NOZOMBIE (rtmux_detach, SPAWN_NOZOMBIE; a child that
     * dies while loading still carries the flag from creation) is never waited
     * for, so marking it ZOMBIE below would pin it until its parent exits.
     * Normal children had the flag cleared at spawn completion.
     */
    if (woke == 0) {
        root_proc_info_t qinfo_fin;

        if (process_query_info(pid, &qinfo_fin) == EOK &&
            (qinfo_fin.flags & RTMUX_FLG_PROC_NOZOMBIE))
            woke++;
    }

    if (woke > 0) {
        process_destroy_call(pid);
    } else {
        root_proc_info_t qinfo_zomb;

        /* No waiter was queued, so the parent either has not called waitpid()
         * yet (it will find the ZOMBIE flag and ask for the reap), or it
         * already did while the cleanup above ran: ProcessExitMark made the
         * exit status visible at the start of the cleanup, and a waitpid() in
         * that window is answered, WAITINFO becomes WAITDONE, and
         * kext_proc_wait_finalize sends no PROC_CODE_TERM because the process
         * is not a zombie yet. Nobody will ask again (WAITDONE hides the child
         * from every later wait), so reap it here. Order matters: set ZOMBIE
         * FIRST, then test WAITDONE. A waiter finalizing before ZOMBIE is set
         * leaves WAITDONE, which the test sees; one finalizing after sees
         * ZOMBIE and sends the notification itself. Both at once only means a
         * second destroy, which fails on the dead pid (proc_terminate path 1
         * and process_destroy_call both tolerate that).
         */
        (void) ProcessFlagsOp(pid, RTMUX_FLG_PROC_ZOMBIE, 0);

        /* A parent destroyed after the check above handed the child to roottask
         * before it was a zombie, so no reap was requested. Roottask never
         * waits for a child, so a zombie it adopted would stay for ever: look
         * again now that ZOMBIE is set and reap it here. The kernel clears
         * ZOMBIE when it takes the reap on itself, so a cleared flag means that
         * is under way and must not be repeated.
         */
        if (process_query_info(pid, &qinfo_zomb) == EOK &&
            ((qinfo_zomb.flags & RTMUX_FLG_PROC_WAITDONE) ||
             (qinfo_zomb.parent_pid == ROOTTASK_PID &&
              (qinfo_zomb.flags & RTMUX_FLG_PROC_ZOMBIE)))) {
            process_destroy_call(pid);
        }
    }

    root_trace("proc_terminate: pid=%d cleanup done\n", pid);
    return 0;
}
