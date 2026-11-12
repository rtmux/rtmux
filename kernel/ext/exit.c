/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Process-exit PRIV callbacks.
 *
 * These bodies run in the kernel so that S-mode instruction fetch into
 * syscall_priv callbacks does not need PTE_U=0 pages in roottask text; roottask
 * (U-mode) reaches them via syscall_priv.
 */

#include <core/globals.h>

#define THR_IN_IPC_WAIT(t) \
    ((t)->state == STATE_REPLY || (t)->state == STATE_SEND)

/* kernel-internal: destroy with explicit process pointer */
extern int _channel_destroy(process_t *prp, int chid);
extern int _timer_destroy(process_t *prp, int id);
extern int _connect_detach(process_t *prp, int coid);

/* kext_exit_destroy_threads — tear down every thread in a dying process.
 *
 * Called from roottask's process_threads_destroy via syscall_priv. Parameters:
 *   arg : process_t * for the dying process
 */
void kext_exit_destroy_threads(void *arg)
{
    process_t *prp = arg;
    int tid;
    thread_t *thp;
    int destroyed = 0;

    /* Defensive guard: refuse to destroy the ROOTTASK_PID (roottask) process.
     * In the U-mode pool model the loader thread (a roottask pool thread) calls
     * _exit() on spawn failure, which routes through proc service's _PROC_EXIT
     * message handler with ctp->info.pid == roottask's pid; the handler then
     * asks the kernel to tear down all of roottask's threads, which is
     * catastrophic. Block the request here.
     */
    if (prp && prp->pid == ROOTTASK_PID) {
        return;
    }
    /* First pass: destroy what we can */
    for (tid = 0; tid < prp->threads.nentries; ++tid) {
        if ((thp = vector_lookup2(&prp->threads, (unsigned) tid))) {
            atomic_or(&thp->flags, RTMUX_FLG_THR_DETACHED);
            thread_destroy(thp);
            destroyed++;
        }
    }

    /* Second pass: thread_destroy can't handle STATE_REPLY/SEND threads
     * (force_ready returns NULL for these states).
     *
     * We must unlink the thread from whatever queue it's on before setting
     * STATE_DEAD, otherwise thread_destroy skips the unlink and leaves a
     * dangling pointer in the reply queue.
     *
     * chp->reply_queue is a LINK2 doubly-linked list (next.thread /
     * prev.thread, **NOT** a priority-ordered pril list): threads are put there
     * via LINKPRIL_BEG in fastmsg.c / message.c. Removing via pril_rem()
     * corrupts the list — pril_rem walks with FLAG_HEAD/FLAG_TAIL semantics
     * that reply_queue entries never acquire — so we unlink inline here.
     *
     * A prior pass may have already unlinked the thread (channel/ connection
     * destroy). A still-linked LINK2 element always has prev set (the unlink
     * below NULLs both next and prev); next is legitimately NULL for the
     * TAIL/sole element of the list. Test prev alone — requiring next too would
     * skip the tail element and leave a destroyed thread dangling on
     * chp->reply_queue; the LINK2_REM below handles a missing next pointer.
     */
    for (tid = 0; tid < prp->threads.nentries; ++tid) {
        if ((thp = vector_lookup2(&prp->threads, (unsigned) tid))) {
            int reply_send;
            int stamped_dead = 0;

        retry_claimed:
            reply_send = THR_IN_IPC_WAIT(thp);

            if (reply_send) {
                connect_t *cop;
                channel_t *chp;

                /* A sender is linked on send/reply_queue and has state set
                 * before block() has restored blocked_on; WAIT_PENDING covers
                 * that window. Read the state and blocked_on only once it is
                 * clear, or a NULL blocked_on from the window reads as "not
                 * queued" and the thread would be stamped dead while linked.
                 */
                while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
                    __asm__ volatile("" ::: "memory");
                cpu_smp_mb();
                cop = (connect_t *) thp->blocked_on;

                /* The state read above is stale by now: a thread that moved on
                 * to STATE_RECEIVE has a channel_t in blocked_on, not a
                 * connect_t.
                 */
                if (cop && TYPE_MASK(cop->type) != TYPE_CONNECTION)
                    cop = NULL;
                chp = cop ? cop->channel : NULL;

                /* The queues and the server's thp->client link are shared with
                 * the channel's live servers and senders on other harts, which
                 * change them under chp->lock. Unlink under that lock, as
                 * force_ready does, and recheck there: a replier or force_ready
                 * that completed the exchange cleared blocked_on, and a thread
                 * claimed with MSG_DELIVERY is unlinked by its claimer (it is
                 * then left to thread_destroy -> force_ready below).
                 */
                if (chp && thp->prev.pril) {
                    while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
                        __asm__ volatile("" ::: "memory");
                    cpu_smp_mb();
                    kspinlock_lock(&chp->lock);
                    if ((thp->state == STATE_REPLY ||
                         thp->state == STATE_SEND) &&
                        thp->blocked_on == cop && thp->prev.pril &&
                        cop->channel == chp &&
                        !(thp->internal_flags & RTMUX_ITF_MSG_DELIVERY)) {
                        thread_t *srv = thp->args.ms.server;

                        if (thp->state == STATE_SEND) {
                            pril_rem(&chp->send_queue, thp);
                        } else {
                            LINKPRIL_REM(thp);

                            /* The server's next receive does
                             * client->args.ms.server = 0; do not leave it a
                             * pointer to this soon-freed thread.
                             */
                            if (srv) {
                                if (srv->client == thp)
                                    srv->client = NULL;
                                thp->args.ms.server = NULL;
                            }
                        }
                        thp->next.thread = NULL;
                        thp->prev.thread = NULL;
                        thp->blocked_on = NULL;

                        /* Stamp the thread dead in the same lock window that
                         * verified it is REPLY/SEND and unlinked it. A stamp
                         * outside the lock acts on the stale state read above:
                         * it can kill a thread that has since moved on (and is
                         * linked on a receive queue), leaving a dead thread on
                         * a queue.
                         */
                        thp->state = STATE_DEAD;
                        stamped_dead = 1;
                        kspinlock_unlock(&chp->lock);

                        /* Unlink-only drop of this dying thread's send/reply
                         * reference during process exit. No last-drop handling
                         * by design (not routed through connect_links_drop):
                         * teardown of the connection itself is owned by
                         * _connect_detach and channel destroy.
                         */
                        (void) atomic_sub_value(&cop->links, 1);
                    } else if (THR_IN_IPC_WAIT(thp) && thp->blocked_on == cop &&
                               thp->prev.pril && cop->channel == chp) {
                        /* Transiently claimed: only MSG_DELIVERY made the
                         * unlink test above fail. The claimer may be a plain
                         * reader that unlinks nothing.
                         *
                         * Do not fall through to thread_destroy: force_ready's
                         * unblock notification may already be consumed by the
                         * claiming server, so nobody would reap the thread and
                         * the process would never finish exiting.
                         *
                         * Drop chp->lock (the claimer's release needs it), wait
                         * out the claim (it spans one transfer), and look
                         * again: the claimer woke the thread, or it is still
                         * ours to unlink.
                         */
                        kspinlock_unlock(&chp->lock);
                        while (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY)
                            __asm__ volatile("" ::: "memory");
                        cpu_smp_mb();
                        goto retry_claimed;
                    } else {
                        kspinlock_unlock(&chp->lock);
                        reply_send = 0;
                    }
                }
            }

            /* Only stamp STATE_DEAD directly for REPLY/SEND — those are the
             * states thread_destroy can't handle (force_ready bails for them)
             * and whose dangling LINK2 pointer we just unlinked inline above.
             *
             * For ALL OTHER states (notably STATE_CONDVAR / SEM / MUTEX left
             * behind by an IPI-deferred destroy in the first pass — see
             * thread_destroy's KILLSELF-deferred return), we MUST go through
             * thread_destroy → force_ready, which does the per-state pril_rem
             * from the sync waitlist. Bypassing force_ready (by pre-stamping
             * STATE_DEAD) leaks the pril entry: the next thread soul allocated
             * at this address is born "still on" a sync waitlist and trips
             * CV_WAIT REENTRY when it next calls cv_wait on that syp.
             */
            if (stamped_dead) {
                /* Atomic: this can race thread_destroy's atomic body decrement
                 * on another hart (a sibling self-reaping at the same time). A
                 * plain -- here would lose an update and leave
                 * prp->num_active_threads > 0 after every thread is gone,
                 * tripping the assertion in kext_process_destroy (process.c).
                 * The other num_active_threads writers (thread_create,
                 * thread_destroy) are already atomic.
                 */
                if (prp->num_active_threads > 0)
                    (void) __atomic_sub_fetch(&prp->num_active_threads, 1,
                                              __ATOMIC_ACQ_REL);
                atomic_or(&thp->flags, RTMUX_FLG_THR_DETACHED);
            }
            thread_destroy(thp);
        }
    }

    LOGF_T("proc_exit: pid=%d destroyed %d threads\n", prp->pid, destroyed);
}

/* Per-process resource teardown bodies.
 *
 * Called from roottask's process_timer_destroy / process_connect_detach via
 * syscall_priv. struct kext_destroy_args lives on the roottask side in
 * proc/kcalls.c and must match the definition here.
 */
struct kext_destroy_args {
    process_t *prp;
    int id;
    int result;
};

/* kext_timer_destroy — destroy one timer on a process by id. Parameters:
 *   arg : struct kext_destroy_args * (prp, id); result returned in result
 */
void kext_timer_destroy(void *arg)
{
    struct kext_destroy_args *kap = arg;
    kap->result = _timer_destroy(kap->prp, kap->id);
}

/* kext_connect_detach — detach one connection on a process by coid. Parameters:
 *   arg : struct kext_destroy_args * (prp, id); result returned in result
 */
void kext_connect_detach(void *arg)
{
    struct kext_destroy_args *kap = arg;
    kap->result = _connect_detach(kap->prp, kap->id);
}

/* kext_proc_destroy_chancons — bulk teardown of chancons entries on a process,
 * filtered by type (TYPE_CHANNEL or TYPE_CONNECTION).
 *
 * Replaces roottask's query_object-then-deref-chp->type loop in proc_exit; that
 * loop dereferenced the kernel-allocated channel_t pointer to read its type,
 * which page-faults under U-mode roottask with heap PTE_U=0. Doing the walk
 * inside the kernel keeps every deref in S-mode where the heap is mapped.
 */
struct kcall_args_proc_destroy_chancons {
    pid_t pid;
    int type_filter; /* TYPE_CHANNEL or TYPE_CONNECTION */
};

void kext_proc_destroy_chancons(void *data)
{
    struct kcall_args_proc_destroy_chancons *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp;
    int status;
    unsigned i;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(prp);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }

    /* prp->chancons is a vector indexed by chid (or scoid for the connection
     * side, with RTMUX_SIDE_CHANNEL OR'd in). Walk every slot; for matching
     * entries, dispatch to the existing per-entry destroy helpers. Tolerates
     * intermediate NULLs (sparse vector).
     */
    for (i = 0; i < prp->chancons.nentries; ++i) {
        channel_t *chp = vector_lookup2(&prp->chancons, i);
        if (!chp)
            continue;
        if (chp->type != kap->type_filter)
            continue;
        if (kap->type_filter == TYPE_CHANNEL) {
            (void) _channel_destroy(prp, (int) i);
        } else {
            (void) _connect_detach(prp, (int) (RTMUX_SIDE_CHANNEL | i));
        }
    }
    process_unpin(prp);
    SETKSTATUS(act, EOK);
}

/* kext_proc_destroy_fdcons — bulk teardown of fdcons entries on a process.
 *
 * fdcons is sparse and _connect_detach removes slots as it goes, so walking it
 * in-kernel avoids roottask's query-object-then-detach race during exit.
 */
struct kcall_args_proc_destroy_fdcons {
    pid_t pid;
};

void kext_proc_destroy_fdcons(void *data)
{
    struct kcall_args_proc_destroy_fdcons *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp;
    int status;
    unsigned i;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(prp);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }

    for (i = 0; i < prp->fdcons.nentries; ++i) {
        connect_t *cop = vector_lookup2(&prp->fdcons, i);

        if (!cop || cop->type != TYPE_CONNECTION)
            continue;
        (void) _connect_detach(prp, (int) i);
    }
    process_unpin(prp);
    SETKSTATUS(act, EOK);
}

/* Notification-based process management.
 *
 * Kernel signals are gone in RTMux (see doc/md/rtmux_signal_removal.md); these
 * are the notification-based replacements for the signal_kill_process(SIGCHLD)
 * / (SIGKILL) call sites the rest of the kernel makes, kept next to the rest of
 * the process-exit plumbing.
 */

/* notification_notify_proc_terminated — notify a parent that a child terminated
 * (replaces signal_kill_process(SIGCHLD)).
 *
 * Called from kext_process_shutdown when a process exits; delivers
 * _NOTIFICATION_CODE_PROC_TERMINATED on the parent's system channel so the
 * parent's two-thread system listener wakes any waitpid() blocker. Parameters:
 *   parent : the parent process to notify (may have no system channel)
 *   prp    : the terminating child process
 */
void notification_notify_proc_terminated(process_t *parent, process_t *prp)
{
    channel_t *chp;

    if (!parent || !prp)
        return;

    rcu_read_lock(&connobj_rcu);
    chp = parent->sys_chp;
    /* Parent has no system channel (e.g. pid 1 during early boot). */
    if (!chp) {
        rcu_read_unlock(&connobj_rcu);
        return;
    }

    notification_deliver(
        chp, prp->terming_priority ? prp->terming_priority : 10,
        _NOTIFICATION_CODE_PROC_TERMINATED, (uintptr_t) prp->pid, -1, 0);
    rcu_read_unlock(&connobj_rcu);
}

/* process_destroy — replaces signal_kill_process(SIGKILL). Called from the
 * debugger's KLC detach path; flags the process for forced destruction and lets
 * the standard kext_exit path do the work.
 */
void process_destroy(process_t *prp, pid_t killer_pid)
{
    thread_t *thp;

    if (!prp)
        return;

    (void) killer_pid;

    /* Find any active thread in the process */
    thp = prp->valid_thp;
    if (!thp)
        return;

    prp->flags |= RTMUX_FLG_PROC_DESTROYALL;
    atomic_or(&thp->flags, RTMUX_FLG_THR_KILLSELF);
}
