/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

/* Interrupt territory. Live kcalls: 55, 57, 58, 59, 60.
 *
 * Shared kernel state touched:
 *   - interrupt_vector (global vector_t, indexed by id)
 *   - interrupt_level[level].queue (global linked list per level)
 *   - interrupt_t fields (next, thread, level, handler, area, flags)
 *   - Per-thread flags / state for the IST wait path
 *   - PLIC mask/unmask via cpu_intr_{mask,unmask} (HW MMIO)
 *
 * Cross-CPU residual: interrupt_vector and interrupt_level chains are mutated
 * by attach/detach without a global slock, so concurrent
 * interrupt_attach_thread / interrupt_detach on different CPUs is a race
 * surface. Practical risk is low (root/IOPRIV-only callers, driver-init-time
 * mostly, infrequent post-boot). The in-kernel-call preemption fence is in
 * place on every kcall body.
 */

static int interrupt_attach_authorized(thread_t *act, uint32_t intr)
{
    if (intr < RTMUX_INTR_CLASS_SYNTHETIC || intr > RTMUX_HOOK_LAST)
        return (act->flags & RTMUX_FLG_THR_IOPRIV) ? EOK : EPERM;

    /* Synthetic interrupts require IRQ capability; loading children hold every
     * cap transiently, so exclude that window.
     */
    if (act->process->flags & RTMUX_FLG_PROC_LOADING)
        return EPERM;

    return (act->process->priv_caps & RTMUX_PRIV_CAP_IRQ) ? EOK : EPERM;
}

/* #55 interrupt_attach_thread — attach the calling thread as an IST.
 *
 * The calling thread becomes the Interrupt Service Thread for the given
 * interrupt vector. It must then call interrupt_wait() in a loop to process
 * interrupts at thread level.
 *
 * The kernel auto-unmasks the interrupt on the first attach (unless
 * RTMUX_INTR_FLAGS_NO_UNMASK is set).
 */
int kdecl
ksys_interrupt_attach_thread(thread_t *act,
                             struct kcall_args_interrupt_attach_thread *kap)
{
    unsigned intr;
    int level;
    int id;

    if (interrupt_attach_authorized(act, kap->intr) != EOK)
        return EPERM;

    if ((level = get_interrupt_level(act, kap->intr)) == -1)
        return ENOERROR;
    intr = (unsigned) level;

    id = interrupt_attach(intr, NULL, NULL, kap->flags);
    if (id >= 0) {
        /* Per-hart IRQ pinning. After interrupt_attach has allocated the
         * interrupt_t with owner_hart=0xFF, claim the current hart as the
         * owning hart for this source. Order matters: record owner_hart and
         * unmask the PLIC source FIRST (while we still have the CPU), THEN call
         * set_runmask(). set_runmask() may reschedule act off-CPU if
         * act->runcpu is stale w.r.t. get_cpunum(); the IRQ delivery must
         * already be live by that point or the IST never sees its first IRQ.
         */
        interrupt_t *itp = vector_lookup(&interrupt_vector, (unsigned) id);
        /* A controller may have a fixed delivery CPU (Pi3 routes all GPU
         * sources together). cpu_intr_attach supplies that constraint. */
        unsigned cpu = itp && itp->owner_hart != 0xff ?
                           itp->owner_hart : get_cpunum();
        if (itp) {
            itp->owner_hart = (uint8_t) cpu;
        }
        {
            extern __attribute__((weak)) void plic_set_owner_hart(
                int irq, unsigned hart);
            if (plic_set_owner_hart) {
                int irq = (int) intr - (int) interrupt_level[intr].level_base;
                plic_set_owner_hart(irq, cpu);
            }
        }

        /* Unmask: PLIC source priority goes from 0 to 1, SENABLE is routed to
         * the owner hart only. Auto-unmask is deferred from interrupt_attach
         * for the IST path without a kernel callback.
         */
        if (!(kap->flags & RTMUX_INTR_FLAGS_NO_UNMASK)) {
            do {
            } while (interrupt_unmask(intr, NULL) != 0);
        }

        /* Pin the IST thread's runmask to the owner hart. This relies on
         * ready_default's cross-CPU re-target fix (dispatch.c) so ready(IST)
         * from any hart correctly routes the IST onto the owner hart's dispatch
         * queue. set_runmask() takes the "can-run-on" mask (not inverted).
         */
        (void) set_runmask(act, 1U << cpu);

        SETKSTATUS(act, id);
    }
    return ENOERROR;
}


/* #56 __interrupt_attach_event — attach a sigevent to an IRQ.
 *
 * Companion to interrupt_attach_thread (#55): instead of waking the calling
 * thread on each IRQ via interrupt_wait, the kernel delivers a COPY of "*event"
 * inline from trap context via sigevent_deliver_inline() — no intrevent_add /
 * drain round-trip. Suitable sigev_notify values: SIGEV_NOTIFICATION,
 * SIGEV_INTR, SIGEV_UNBLOCK, SIGEV_NONE. SIGEV_THREAD is rejected at
 * registration (thread_create is unsafe in trap context).
 *
 * The calling thread's process is recorded as the owner so per-process cleanup
 * on exit works identically to interrupt_attach_thread.
 */
int kdecl
ksys_interrupt_attach_event(thread_t *act,
                            struct kcall_args_interrupt_attach_event *kap)
{
    unsigned intr;
    int level;
    int id;

    if (interrupt_attach_authorized(act, kap->intr) != EOK)
        return EPERM;

    if (!kap->event)
        return EINVAL;

    if ((level = get_interrupt_level(act, kap->intr)) == -1)
        return ENOERROR;
    intr = (unsigned) level;

    id = interrupt_attach_event(intr, kap->event, kap->flags);
    if (id >= 0) {
        /* Per-hart IRQ pinning. Mirror of the THREAD path above EXCEPT we do
         * not touch the caller's runmask: the event-delivery target (sigevent's
         * receiver) is an independent thread whose affinity is managed
         * separately.
         */
        interrupt_t *itp = vector_lookup(&interrupt_vector, (unsigned) id);
        unsigned cpu = itp && itp->owner_hart != 0xff ?
                           itp->owner_hart : get_cpunum();
        if (itp) {
            itp->owner_hart = (uint8_t) cpu;
        }
        {
            extern __attribute__((weak)) void plic_set_owner_hart(
                int irq, unsigned hart);
            if (plic_set_owner_hart) {
                int irq = (int) intr - (int) interrupt_level[intr].level_base;
                plic_set_owner_hart(irq, cpu);
            }
        }

        /* Auto-unmask was removed from interrupt_attach_event; do it here, now
         * that owner_hart is recorded.
         */
        if (!(kap->flags & RTMUX_INTR_FLAGS_NO_UNMASK)) {
            do {
            } while (interrupt_unmask(intr, NULL) != 0);
        }

        SETKSTATUS(act, id);
    }
    return ENOERROR;
}


/* #57 interrupt_detach — detach an interrupt handler by id. */
int kdecl ksys_interrupt_detach(thread_t *act,
                                struct kcall_args_interrupt_detach *kap)
{
    interrupt_t *itp;

    if (!(act->flags & RTMUX_FLG_THR_IOPRIV))
        return EPERM;

    itp = vector_lookup(&interrupt_vector, kap->id);
    if (!itp) {
        return ESRCH;
    }

    if (itp->thread->process != act->process) {
        return EPERM;
    }

    interrupt_detach_entry(act->process, kap->id);

    return EOK;
}


/* #59 __interrupt_mask — mask an interrupt level via PLIC. */
int kdecl ksys_interrupt_mask(thread_t *act,
                              struct kcall_args_interrupt_mask *kap)
{
    int status;
    int level;

    if (!(act->flags & RTMUX_FLG_THR_IOPRIV))
        return EPERM;

    if ((level = get_interrupt_level(act, kap->intr)) == -1)
        return ENOERROR;

    status = interrupt_mask((unsigned) level,
                            vector_lookup(&interrupt_vector, kap->id));
    if (status == -1)
        kcall_err(act, EINVAL);
    SETKSTATUS(act, status);
    return ENOERROR;
}


/* #60 __interrupt_unmask — unmask an interrupt level via PLIC. */
int kdecl ksys_interrupt_unmask(thread_t *act,
                                struct kcall_args_interrupt_unmask *kap)
{
    int status;
    int level;

    if (!(act->flags & RTMUX_FLG_THR_IOPRIV))
        return EPERM;

    if ((level = get_interrupt_level(act, kap->intr)) == -1)
        return ENOERROR;

    status = interrupt_unmask((unsigned) level,
                              vector_lookup(&interrupt_vector, kap->id));
    if (status == -1)
        kcall_err(act, EINVAL);
    SETKSTATUS(act, status);
    return ENOERROR;
}


/* #58 interrupt_wait — IST blocks here until SIGEV_INTR delivers an interrupt
 * event to act->flags |= RTMUX_FLG_THR_INTR_PENDING.
 *
 * The PENDING-clear is atomic_clr; the wait path uses unready+block
 * (sched_slock-internal in dispatch.c). No additional locking needed in the
 * kcall body: the in-kernel-call preemption fence covers the canstub-redirect
 * and the state mutation prior to block. Walk interrupt_vector and unmask every
 * IRQ level where the calling thread is the registered IST. Used by
 * RTMUX_INTR_WAIT_FLAGS_UNMASK so the IST can collapse the common
 * "__interrupt_unmask(); interrupt_wait();" pair into one syscall.
 *
 * "interrupt_unmask" cascades into the PLIC's per-source unmask callback, which
 * in turn calls plic_complete_deferred — so the deferred-claim recorded by
 * plic_record_claim in the trap path is completed here as well (no separate
 * plic_complete needed).
 *
 * In typical use a thread is attached to one IRQ, so this walk is tiny. If a
 * thread is attached to many IRQs, unmasking all of them is the right semantics
 * — interrupt_wait without an explicit id can't pick which one.
 */
static void interrupt_unmask_for_thread(thread_t *act)
{
    unsigned n = interrupt_vector.nentries;
    unsigned i;
    for (i = 0; i < n; i++) {
        interrupt_t *itp = vector_lookup(&interrupt_vector, i);
        if (itp && itp->thread == act && itp->kind == INTR_KIND_THREAD) {
            (void) interrupt_unmask(itp->level, itp);
        }
    }
}

/* #58 interrupt_wait — block the calling IST until its IRQ fires.
 *
 * Returns immediately if RTMUX_FLG_THR_INTR_PENDING is already set; otherwise
 * (optionally unmasking attached levels first) transitions to STATE_INTR and
 * blocks, with a race-close re-check of the pending bit after the state commit.
 * Parameters:
 *   act : the calling (active) thread (the IST)
 *   kap : kcall args (flags: UNMASK / FAST, optional timeout)
 * Return value: EOK on wake; ETIMEDOUT / ENOTSUP / ENOERROR per flags.
 */
int kdecl ksys_interrupt_wait(thread_t *act,
                              struct kcall_args_interrupt_wait *kap)
{
    unsigned wflags = kap->flags;
    int unmask_requested = (wflags & RTMUX_INTR_WAIT_FLAGS_UNMASK) ? 1 : 0;
    int fast = (wflags & RTMUX_INTR_WAIT_FLAGS_FAST) ? 1 : 0;

    if (PENDCAN(TLS_FLAGS_SAFE(act))) {
        SETKIP_FUNC(act, act->process->canstub);
        return ENOERROR;
    }

    if (act->flags & RTMUX_FLG_THR_INTR_PENDING) {
        /* Atomic clear — paired with atomic_or in event.c SIGEV_INTR delivery.
         * Preserves cross-CPU ordering against other paths that
         * read-modify-write act->flags (force_ready KILLSELF set, exec
         * KCALL_ERR_SET set, etc.).
         */
        atomic_clr((volatile unsigned *) &act->flags,
                   RTMUX_FLG_THR_INTR_PENDING);
        act->timeout_flags = 0;
        CRITICAL_MARK_THREAD(act);

        /* If the caller asked us to unmask, do it on the fast-path return too —
         * drivers using "for (;;) interrupt_wait(UNMASK|FAST, NULL)" expect the
         * unmask side-effect every time, not just when we block.
         */
        if (unmask_requested) {
            interrupt_unmask_for_thread(act);
        }
        _TRACE_IST_DISPATCH(NULL, act);
        return EOK;
    }

    /* RTMUX_INTR_WAIT_FLAGS_UNMASK: re-unmask attached IRQ levels before
     * blocking, so the next IRQ can arrive. The interrupt_wait UNMASK flag is
     * the driver's threaded-IRQ re-enable point and subsumes a prior explicit
     * __interrupt_unmask call. Must happen AFTER the INTR_PENDING fast-path
     * check above (so a pending bit still wins) and BEFORE the state transition
     * + block below (so a freshly delivered IRQ sees STATE_INTR and uses
     * ready()).
     */
    if (unmask_requested) {
        interrupt_unmask_for_thread(act);
    }

    /* RTMUX_INTR_WAIT_FLAGS_FAST: skip timeout setup entirely. No timeout
     * pointer is consulted, no IMTO probe is done. Standard path retains
     * existing semantics.
     */
    if (!fast) {
        if (kap->timeout) {
            return ENOTSUP;
        }
        if (IMTO(act, STATE_INTR)) {
            return ETIMEDOUT;
        }
    }

    SETKSTATUS(act, 0);
    act->state = STATE_INTR;
    _TRACE_TH_EMIT_STATE(act, INTR);

    /* Race-close re-check: an IRQ trap fired on THIS hart between the
     * INTR_PENDING check (line ~243) and the STATE_INTR commit above would have
     * observed "act" in STATE_RUNNING (== "here" on the trap-handler same-hart
     * check) and taken the atomic_or PENDING path. Re-check after the state
     * commit so we don't block() with PENDING already set.
     *
     * IRQ-pinning invariant: same-hart program order is sufficient here. The
     * trap handler runs on the SAME hart as this IST (the source is SENABLE'd
     * only on this hart via plic_owner_hart[] and this thread's runmask pins it
     * here). No cross-hart memory fence is required — a compiler barrier keeps
     * the C compiler from reordering the state write past the PENDING read;
     * hardware program order does the rest.
     */
    __asm__ __volatile__("" ::: "memory");
    if (act->flags & RTMUX_FLG_THR_INTR_PENDING) {
        atomic_clr((volatile unsigned *) &act->flags,
                   RTMUX_FLG_THR_INTR_PENDING);
        act->state = STATE_RUNNING;
        act->timeout_flags = 0;
        CRITICAL_MARK_THREAD(act);
        _TRACE_IST_DISPATCH(NULL, act);
        return EOK;
    }

    /* block() is sched_slock-internal; preempt fence drops below the tail call
     * by symmetry with the receive path (block doesn't return on the same kcall
     * stack — the new active runs after trap exit). Equivalent to the BKL idiom
     * of dropping the lock pre-block.
     */
    block();
    _TRACE_IST_DISPATCH(NULL, act);
    return EOK;
}
