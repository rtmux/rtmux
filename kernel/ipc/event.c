/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#ifdef CONFIG_DEBUG_INJECT
#include <private/inject.h>
#endif

/* RTMux direct notification delivery.
 *
 * Sigevent producers (PLIC trap, STIMER trap, timer_activate kernel extension,
 * trace events) deliver inline via sigevent_deliver_inline() — there is no
 * softirq, workqueue, deferred intrevent queue, free-list, or drain.
 * Cross-references:
 *   - arch/riscv/trap-handler.c PLIC + STIMER branches
 *   - kernel/time/timer-core.c timer_fire + timer_activate
 *   - kernel/trace/core.c trace event delivery
 *
 * This file provides:
 *   - sigevent_deliver_inline() — the producer-facing helper.
 *   - sigevent_exe() — the per-sigev_notify dispatcher; also used
 *     by sync event delivery.
 *   - sigevent_proc() — process-level sigevent validation.
 *   - sigevent_notification_roottask() — direct kernel→roottask notification
 * path.
 *   - intrevent_flush() — cross-CPU wait for in-flight IRQs to
 *     complete (an INKERNEL_INTRMASK spin).
 *   - intrevent_preemption() — preempt-signal entry from
 *     clock_handler; writes per-CPU pending_preempt_prio.
 */


/* intrevent_preemption — signal "a higher-priority thread is now ready, please
 * preempt the current kernel extension mid-loop if it's a long-running one."
 * Called from clock_handler when the time- slice expires and a different thread
 * should run.
 *
 * Writes this CPU's pcpu_array[cpu].pc_pending_preempt_prio; NEED_PREEMPT (in
 * macros.h) reads it. Parameters:
 *   prio : priority of the now-ready thread (clamped to 0xFF)
 */
void intrevent_preemption(unsigned prio)
{
    if (!kopt.nopreempt) {
        unsigned cpu = get_cpunum();
        if (prio > 0xFF)
            prio = 0xFF;
        if ((uint8_t) prio > pcpu_array[cpu].pc_pending_preempt_prio)
            pcpu_array[cpu].pc_pending_preempt_prio = (uint8_t) prio;
    }
}


/* sigevent_deliver_inline — helper for the direct-from-trap notification
 * delivery model. Called from:
 *   - arch/riscv/trap-handler.c PLIC branch (per attached
 *     interrupt_t, for INTR_KIND_THREAD via a synthetic SIGEV_INTR
 *     and for INTR_KIND_EVENT via the registered itp->event).
 *   - arch/riscv/trap-handler.c STIMER branch (via
 *     timer-core.c::timer_fire).
 *   - kernel/time/timer-core.c::timer_activate (kernel extension context —
 *     past-deadline absolute timer).
 *
 * Calls sigevent_exe(evp, target, 1) directly — sigevent_exe handles every
 * sigev_notify kind correctly:
 *   - SIGEV_INTR  → ready() or atomic_or INTR_PENDING.
 *   - SIGEV_UNBLOCK → ready() or force_ready(ETIMEDOUT).
 *   - SIGEV_NOTIFICATION → notification_deliver() (channel_slock).
 *   - SIGEV_NONE  → no-op (or timer_pending() for the idle thread).
 *   - SIGEV_THREAD → sigevent_exe checks INKERNEL_NOW and returns -1
 *     when called from trap context (skipping thread_create), so it is
 *     dropped here.  This is fine because SIGEV_THREAD is rejected at
 *     registration (__interrupt_attach_event and ksys_timer_* return EINVAL),
 *     so it cannot reach trap context via these producers.
 *   - SIGEV_SIGNAL* — already EINVAL (no-signal rule in RTMux).
 *
 * Invariants the caller must honour:
 *   1. INKERNEL_NOW is NOT set in trap context.  notification_deliver
 *      observes that and uses the buffered (atomic_or
 *      RTMUX_FLG_THR_NOTIFICATION) path — no user-page touch, no fault risk.
 *      In kernel extension context (timer_activate of a past-deadline absolute
 *      timer) INKERNEL_NOW IS set; the inline xfer path is correct.
 *   2. No lock is held at entry.  Downstream lock acquisitions
 *      (sched_slock via ready/force_ready, channel_slock via
 *      notification_deliver, sync_slock via force_ready STATE_CONDVAR/
 *      MUTEX/SEM) are rank-disjoint from the trap-entry state.
 */
void sigevent_deliver_inline(const struct sigevent *evp,
                             thread_t *target,
                             int reserved)
{
    if (!evp || !target)
        return;
    (void) sigevent_exe(evp, target, 1, reserved);

    /* Preempt-sentinel: bump this CPU's pending_preempt_prio if the wake just
     * made a higher-priority thread runnable. Long-running kernel extension
     * loops on this hart (xfer copy, timer batch fire, ext/misc and ext/debug
     * iteration) consult NEED_PREEMPT each iteration and KCALL_RESTART when
     * they see a pending preemption, yielding to the dispatch.
     * Single-writer-by-cpu (this trap on this hart only writes this hart's
     * slot), no atomic needed.
     */
    {
        unsigned cpu = get_cpunum();
        uint8_t prio = target->priority;
        if (prio > pcpu_array[cpu].pc_pending_preempt_prio)
            pcpu_array[cpu].pc_pending_preempt_prio = prio;
    }
}

/* intrevent_flush — wait for in-flight IRQ handlers on remote CPUs to exit so
 * the caller can safely destroy state the IRQ path touches (thread_t,
 * interrupt_t). Called from thread_destroy (lifecycle.c) and
 * interrupt_detach_entry (core.c).
 *
 * Spins on the cross-CPU INKERNEL_INTRMASK bit.
 */
void intrevent_flush(void)
{
#if CONFIG_PROCESSORS_MAX > 1
#ifndef NDEBUG
    unsigned start = (unsigned) qtimeptr->nsec;
#ifdef CONFIG_DEBUG_INJECT
    unsigned long long inj_token = inject_stall_token();
#endif
#endif
    for (;;) {
        MEM_BARRIER_RW();
        if ((get_inkernel() & INKERNEL_INTRMASK) == 0)
            break;

        /* Briefly enable interrupts during the spin so pending IPIs (notably
         * IPI_CRASH from a sibling that has crashed) can deliver as traps.
         * Re-disable immediately to match the surrounding kernel-state
         * expectations.
         */
        interrupt_enable();
        interrupt_disable();
#ifndef NDEBUG
        if (((unsigned) qtimeptr->nsec - start) > 1000000000
#ifdef CONFIG_DEBUG_INJECT
            && !inject_stall_overlapped(inj_token)
#endif
        ) {
            crash();
        }
#ifdef CONFIG_DEBUG_INJECT
        /* An injected stall explains the wait so far: start the one-second
         * budget afresh, so an unrelated stuck flush after it still trips.
         */
        if (((unsigned) qtimeptr->nsec - start) > 1000000000 &&
            inject_stall_overlapped(inj_token)) {
            start = (unsigned) qtimeptr->nsec;
            inj_token = inject_stall_token();
        }
#endif
#endif
    }
#endif
}


/* sigevent_exe — execute a sigevent against a target thread.
 *
 * Dispatches on sigev_notify kind (SIGEV_NONE / SIGEV_THREAD /
 * SIGEV_NOTIFICATION / SIGEV_UNBLOCK / SIGEV_INTR; signal kinds are rejected),
 * either delivering the event or just validating it. May be called directly
 * from an interrupt handler. Parameters:
 *   evp      : the sigevent to execute
 *   thp      : target thread
 *   deliver  : non-zero to actually deliver, zero to validate only
 *   reserved : non-zero when the source reserved a notification soul, so a
 *              SIGEV_NOTIFICATION delivery uses the allocation-free reserve
 * Return value: EOK on success, -1 if delivery must be deferred (called
 *               from interrupt context for a kind that needs allocation),
 *               or an errno.
 */
int rdecl sigevent_exe(const struct sigevent *evp,
                       thread_t *thp,
                       int deliver,
                       int reserved)
{
    connect_t *cop;
    channel_t *chp;
    process_t *prp;
    int status;
    unsigned prio;

    switch (SIGEV_GET_TYPE(evp)) {
    case SIGEV_NONE:
        if (thp == kernel_threads_prp->threads.vector[0]) {
            if (!(get_inkernel() & INKERNEL_NOW)) {
                /* If we're being invoked by an interrupt handler, we must queue
                 * the event - timer_pending() needs to be able to free memory.
                 */
                return -1;
            }

            /* If the thread that signaled the event is the idle thread, that
             * was an indicator that we have pending timer operations that need
             * to be dealt with.
             */
            timer_pending();
        }
        break;

    case SIGEV_SIGNAL_THREAD:
    case SIGEV_SIGNAL:
        /* Signal events removed — use SIGEV_NOTIFICATION instead */
        return EINVAL;

    case SIGEV_THREAD:
        if (thp->process->flags & RTMUX_FLG_PROC_DESTROYALL) {
            return EBUSY;
        }
        if (!(get_inkernel() & INKERNEL_NOW)) {
            /* If we're being invoked by an interrupt handler, we must queue the
             * event - thread_create() might need to allocate memory.
             */
            return -1;
        }
        if (!deliver ||
            (status = thread_create(thp, thp->process, evp,
                                    evp->sigev_notify & SIGEV_FLAG_CRITICAL
                                        ? THREAD_CREATE_CRITICAL_FLAG
                                        : 0,
                                    NULL)) == ENOERROR) {
            status = EOK;
        }
        return status;

    case SIGEV_NOTIFICATION: {
        vector_t *vec;
        unsigned coid = (unsigned) evp->sigev_coid;

        prp = thp->process;
        vec = &prp->fdcons;
        if (coid & RTMUX_SIDE_CHANNEL) {
            coid &= ~RTMUX_SIDE_CHANNEL;
            vec = &prp->chancons;
        }

        if (vec && (coid < vec->nentries) &&
            (VECAND(cop = VEC(vec, coid), 1) == 0)) {
            cop = VECAND(cop, ~3);
        } else {
            cop = NULL;
        }

        if (!cop || cop->type != TYPE_CONNECTION) {
            return ESRCH;
        }

        /* Re-check the connection's send right at delivery, not only when the
         * sigevent was registered, so a client that armed a notification and
         * then had its send right revoked (RTMUX_FLG_COF_REVOKE_SEND through
         * connect_flags) stops delivering. This closes the register-then-revoke
         * window. cop->rights is cleared with an atomic on the revoke path, so
         * read it with one here.
         */
        if ((__atomic_load_n(&cop->rights, __ATOMIC_RELAXED) &
             RTMUX_CAP_ENDPOINT_SEND) == 0) {
            return EPERM;
        }
        if (!(chp = cop->channel)) {
            return EBADF;
        }

        if (deliver) {
            /* A -1 notification priority means use the priority of the
             * *process* of the receiver
             */
            prio = (evp->sigev_priority == -1)
                       ? chp->process->process_priority
                       : (unsigned) evp->sigev_priority &
                             _NOTIFICATION_PRIO_PUBLIC_MASK;
            _TRACE_COMM_EMIT_SNOTIFICATION_EXE(
                cop, cop->scoid | RTMUX_SIDE_CHANNEL, prio);
            if ((status = notification_deliver(
                     chp, prio, evp->sigev_code,
                     (uintptr_t) evp->sigev_value.sival_int,
                     cop->scoid | RTMUX_SIDE_CHANNEL,
                     (evp->sigev_notify & SIGEV_FLAG_CRITICAL
                          ? NOTIFICATION_DELIVER_CRITICAL_FLAG
                          : 0) |
                         (reserved ? NOTIFICATION_DELIVER_RESERVED_FLAG
                                   : 0)))) {
                return status;
            }
        }
        break;
    }

    case SIGEV_UNBLOCK:
        if (deliver) {
            /* a critical unblock event seems unlikely, but wth. */
            if (evp->sigev_notify & SIGEV_FLAG_CRITICAL)
                CRITICAL_MARK_THREAD(thp);

            if (thp->state == STATE_NANOSLEEP) {
                ready(thp);
            } else {
                force_ready(thp, ETIMEDOUT);
            }
        }
        break;

    case SIGEV_INTR:
        if (deliver) {
            if (thp->state == STATE_INTR) {
                if (evp->sigev_notify & SIGEV_FLAG_CRITICAL)
                    CRITICAL_MARK_THREAD(thp);
                ready(thp);
            } else {
                /* Atomic OR — paired with atomic_clr in ksys_interrupt_wait
                 * (kernel/irq/interrupt.c). The atomic + atomic pairing lets
                 * ksys_interrupt_wait run BKL-less without losing cross-CPU
                 * INTR_PENDING set/clear ordering against other paths that
                 * read-modify-write thp->flags (force_ready KILLSELF set, exec
                 * KCALL_ERR_SET set, etc.).
                 */
                atomic_or((volatile unsigned *) &thp->flags,
                          RTMUX_FLG_THR_INTR_PENDING);
            }
        }
        break;

    default:
        return EINVAL;
    }


    return EOK;
}

/* sigevent_proc — execute a sigevent against the proc service.
 *
 * Targets the first (idle) thread of kernel_threads_prp, which always exists.
 * Parameters:
 *   evp : the sigevent to execute (NULL returns -1)
 * Return value: result of sigevent_exe, or -1 if evp is NULL.
 */
int rdecl sigevent_proc(const struct sigevent *evp)
{
    if (evp) {
        CRASHCHECK(SIGEV_GET_TYPE(evp) == SIGEV_NOTIFICATION &&
                   evp->sigev_priority == -1);
        return sigevent_exe(evp, kernel_threads_prp->threads.vector[0], 1, 0);
    }

    return -1;
}

/* sigevent_notification_roottask — kernel-side direct notification to
 * roottask's process channel. Bypasses sigevent_exe()'s SIGEV_NOTIFICATION
 * connection lookup, which resolves ROOTTASK_COID through
 * "kernel_threads_prp->chancons[0]".
 *
 * "kernel_threads_prp" is a dangling pseudo-process: roottask.c installs the
 * real U-mode-roottask process_t in process_vector[ROOTTASK_PID], and the
 * pseudo-process's chancons[0] was never wired to anything, so
 * sigevent_proc(SIGEV_NOTIFICATION → ROOTTASK_COID) returns ESRCH. This helper
 * locates roottask via its pid slot and notification_deliver()s straight into
 * its process channel — no connection-table indirection. Parameters:
 *   code     : notification code
 *   priority : notification priority
 *   value    : notification value
 * Return value: result of notification_deliver, or ESRCH/EBADF if roottask's
 *               channel can't be located.
 */
int rdecl sigevent_notification_roottask(int code,
                                         unsigned priority,
                                         uintptr_t value)
{
    process_t *prp;
    void *obj;
    channel_t *chp;

    if (!(prp = vector_lookup(&process_vector, ROOTTASK_PID)))
        return ESRCH;

    if (!(obj = vector_lookup(&prp->chancons, ROOTTASK_CHID)))
        return ESRCH;

    /* roottask owns the process channel locally, so chancons[ROOTTASK_CHID] is
     * the channel_t itself. Fall back to the connect_t path in case the slot is
     * ever rewired through a connection.
     */
    if (((channel_t *) obj)->type == TYPE_CHANNEL) {
        chp = (channel_t *) obj;
    } else if (((connect_t *) obj)->type == TYPE_CONNECTION) {
        chp = ((connect_t *) obj)->channel;
        if (!chp)
            return EBADF;
    } else {
        return ESRCH;
    }

    return notification_deliver(chp, priority, code, value,
                                (intptr_t) RTMUX_SIDE_CHANNEL, 0);
}
