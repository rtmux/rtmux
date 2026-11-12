/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <stdatomic.h>

#define RESCHED_TIMESLICE 0x1 /* Timeslice is exhausted */
#define RESCHED_SCHEDULE 0x2  /* Scheduling parameters changed globally */
#define RESCHED_THREAD 0x4    /* This thread needs to be re-scheduled */

/* See clock_start() for details on when we can get rid of these temp variables
 */
static volatile uint64_t stable;
volatile uint64_t *nssptr = &stable;

/* Per-hart supervisor-timer interrupt tally, incremented in the RISC-V STIMER
 * trap path. Each hart writes only its own slot, but the boot CPU reads every
 * slot for the tickless-idle diagnostic below, so use relaxed atomics to keep
 * the cross-hart access race-free. Diagnostic only; ordering does not matter.
 */
_Atomic uint64_t stimer_intr_count[CONFIG_PROCESSORS_MAX];

/* Per-tick clock interrupt handler.
 *
 * Runs the stall detector, accounts running/system time to this CPU's active
 * thread, applies sporadic-scheduler bookkeeping and round-robin time-slice
 * preemption, advances global time (boot CPU only) and fires expired timers.
 * Parameters:
 *   dummy : unused interrupt-handler area pointer
 *   id    : unused interrupt id
 * Return value: always NULL (no deferred sigevent to deliver).
 */
const struct sigevent *clock_handler(void *dummy, int id)
{
    if (kopt.disable_timer) {
        /* --disable-timer verification — this must not fire. */
        kprintf("!!! clock_handler fired on cpu %u — disable-timer leak !!!\n",
                get_cpunum());
        crash();
    }
    systime_t *qtp = qtimeptr;
    unsigned my_cpu;
    unsigned int my_inkernel;
    int hi_pri;
    unsigned reschedl;
    unsigned preempt_prio;
    uint64_t elapsed_nsec;

    /* If the hardware doesn't keep a free-running timer or automatically re-arm
     * the interrupt, the startup code will set the timer_reload callout address
     * to non-NULL. Silly way to design the system, since the routine will have
     * to deal with clock skew, but there you go. The kernel debugger also grabs
     * this and points it to code within itself when it's present - it's used
     * for check for async stop requests.
     */
    elapsed_nsec = qtp->nsec_inc;
    if (get_cpunum() == boot_cpunum && qtp->timer_scale == -9) {
        /* Dynamic-tick mode (timer_scale == -9, matching arch_timer_now_ns):
         * arch_timer_now_ns() tracks real elapsed time, so bill the interval
         * that actually passed. If the clock did not advance (a settime/adjust
         * edge), bill nothing rather than a phantom tick. Periodic mode and
         * non-boot CPUs keep the fixed nsec_inc set above — those ticks are
         * exactly one period apart.
         */
        uint64_t now_nsec = arch_timer_now_ns(qtp);
        elapsed_nsec = now_nsec > qtp->nsec ? now_nsec - qtp->nsec : 0;
    }
    if (!arch_timer_reload(qtp))
        return NULL;

    /* Stall detector runs before the in_kernel_context() bailout so a hang with
     * the current CPU inside the kernel is still caught.
     *
     * stall_detector_tick() itself is LOCK-FREE — a volatile read of
     * heartbeat_seq and an atomic CAS on the once-per-wedge "fired" flag. The
     * kprintf burst lives in stall_dump(), which is gated by that CAS so it
     * runs on at most one hart and only once per wedge. Safe to call from
     * timer-interrupt context with BKL held.
     */
    stall_detector_tick();

    /* If THIS CPU is inside the kernel, bail out. The timer interrupt shares
     * the kernel stack with the interrupted kernel call — same-CPU reentrancy
     * would corrupt the stack frame.
     */
    if (in_kernel_context())
        return NULL;

#if defined(CONFIG_COUNT_CYCLES)
    CONFIG_COUNT_CYCLES(qtp->timer_load, &cycles);
#endif

    reschedl = 0;
    if (ss_replenish_list && ss_replenish_time <= (qtp->nsec + elapsed_nsec)) {
        reschedl |= RESCHED_SCHEDULE;
    }

    my_cpu = get_cpunum();
    my_inkernel = get_inkernel();

    if (scheduler_tick_hook) {
        if (scheduler_tick_hook()) {
            reschedl |= RESCHED_SCHEDULE;
        }
        hi_pri = NUM_PRI;
    } else {
        kdispatch_t *dpp0 = pcpu_array[my_cpu].pc_dispatch;
        hi_pri = dpp0 ? DISPATCH_HIGHEST_PRI(dpp0) : 0;
    }

    preempt_prio = 0;

    /* Per-CPU thread accounting. On RISC-V each hart has its own independent
     * timer (mtimecmp via sbi_set_timer), so each CPU accounts only its own
     * active thread — no cross-CPU reach into remote threads' act->reg.
     *
     * One cross-hart hazard survives that per-CPU split, though: a foreign-tid
     * sched_thread() or thread_destroy() running on another hart can free (or
     * realloc) this thread's schedinfo.ss_info while we are mid-access here.
     * sched_ss_cleanup() closes that window against this handler with an
     * RCU-sched grace period (see the SPORADIC block below); ticker_preamble
     * (the old cross-CPU-preamble half of the retired handshake) is gone.
     */
    {
        thread_t *act = get_active_thread(my_cpu);
        if (act && act->state == STATE_RUNNING) {
            /* Bill the stretch since the last switch/tick. Owner-hart write
             * (this hart's active thread); see sched_account_current().
             */
            {
                uint64_t now;
                snap_time(&now, 0);
                sched_account_current(now);
            }

            if (kcall_op_clock_handler_hook) {
                kcall_op_clock_handler_hook(act);
            }

            if ((my_inkernel & INKERNEL_NOW) &&
                ((act->process->pid != ROOTTASK_PID) ||
                 (act->tid >= num_processors))) {
                act->process->system_time += elapsed_nsec;
            }

            /* This tick reads act's schedinfo.ss_info for a SPORADIC thread,
             * which a foreign-hart sched_ss_cleanup() can free. No per-tick
             * handshake is needed: the clock runs in interrupt context and
             * cannot be preempted mid-access, so it is implicitly an RCU-sched
             * read-side section — sched_ss_cleanup()'s rcu_sched_synchronize()
             * waits for this handler to finish (its CPU reaches a quiescent
             * state only on interrupt return) before it frees ss_info. The
             * preemptible lockless readers (resched_default, sched_get) can
             * pass a quiescent state mid-access, so they still take the
             * ticker_using handshake; this hot path does not pay for it.
             */

            /* Charge the round-robin timeslice by the time that actually
             * elapsed, not a flat tick. In periodic mode elapsed_nsec ==
             * nsec_inc so rr_add == RR_FULLTICK (identical to the old
             * RR_ADD_FULLTICK); in dynamic-tick mode the boot CPU sees
             * sub-period interrupts, and a flat charge would deplete an RR
             * slice far too fast under timer-heavy load. Saturates at
             * RR_MAXTICKS like the macro did. schedinfo is a union: rr_ticks
             * (round-robin timeslice) overlays ss_info (the sporadic pointer),
             * and rr_ticks is consumed only by the IS_SCHED_RR path — so charge
             * it strictly for RR/OTHER. Charging any other policy writes the
             * union for a thread that does not own rr_ticks: for SPORADIC it
             * clobbers the ss_info pointer the accounting below then derefs,
             * and for the brief FIFO window inside a sched_thread() transition
             * into SPORADIC (policy is FIFO until ss_info is installed and
             * published) it clobbers the freshly stored pointer. IS_SCHED_RR
             * excludes both.
             */
            if (IS_SCHED_RR(act)) {
                /* Snapshot nsec_inc once: clock_resolution() on another CPU
                 * could change it between the two uses, skewing the divide.
                 */
                uint64_t nsec_inc = qtp->nsec_inc;
                uint64_t rr_add =
                    (elapsed_nsec * RR_FULLTICK + nsec_inc / 2) / nsec_inc;

                /* Compare in 64-bit before narrowing — a huge elapsed (e.g.
                 * across suspend) must saturate, not wrap to a small add.
                 */
                act->schedinfo.rr_ticks =
                    (rr_add >= RR_MAXTICKS ||
                     act->schedinfo.rr_ticks > RR_MAXTICKS - (unsigned) rr_add)
                        ? RR_MAXTICKS
                        : act->schedinfo.rr_ticks + (unsigned) rr_add;
            }

            reschedl &= ~(unsigned) (RESCHED_THREAD | RESCHED_TIMESLICE);

            if (act->policy == SCHED_SPORADIC) {
                /* Load the union pointer once. The RCU-sched grace period in
                 * sched_ss_cleanup() keeps ss alive until this interrupt
                 * returns, and cleanup sets policy to SCHED_FIFO before that
                 * grace period, so a live SPORADIC read here sees a valid ss;
                 * the NULL guard is belt-and-suspenders against a future path
                 * that leaves a SPORADIC thread without an ss_info.
                 */
                sched_info_t *ss = act->schedinfo.ss_info;

                /* consumed/curr_budget were already charged by
                 * sched_account_current() above; the tick only detects
                 * exhaustion (max one tick late, as before).
                 */
                if (ss && ss->org_priority == 0 && ss->curr_budget == 0)
                    reschedl |= RESCHED_SCHEDULE;
            }

            if (!kopt.nopreempt && IS_SCHED_RR(act)) {
                if (act->schedinfo.rr_ticks >= RR_MAXTICKS) {
                    reschedl |= RESCHED_TIMESLICE;
                } else if (hi_pri >= act->priority) {
                    reschedl |= RESCHED_TIMESLICE;
                }
            }

            if (reschedl) {
                if ((reschedl & RESCHED_TIMESLICE) && hi_pri >= act->priority) {
                    switch (act->policy) {
                    case SCHED_RR:
                    case SCHED_OTHER:
                        reschedl |= RESCHED_THREAD;
                        break;
                    default:
                        break;
                    }
                }
            }

            /* RLIMIT_CPU is checked for every ticking thread, not only when
             * there is something to reschedule: a lone spinner has reschedl ==
             * 0 and would otherwise never be stopped. SMP invariant: this hart
             * writes only its own active thread's async_flags, and reads the
             * process's running_time (summed atomically by every hart that
             * bills one of its threads, including the stretch billed just
             * above) and max_cpu_time (stored atomically by
             * kext_proc_set_rlimit), so a multi-hart process is stopped by
             * whichever of its harts ticks first past the limit and no hart
             * needs another's lock. The flag makes the thread enter
             * resched_default, which sets KILLSELF.
             */
            {
                uint64_t cpu_max = __atomic_load_n(&act->process->max_cpu_time,
                                                   __ATOMIC_RELAXED);
                int cpu_over =
                    cpu_max != 0 && __atomic_load_n(&act->process->running_time,
                                                    __ATOMIC_RELAXED) > cpu_max;

                if (cpu_over ||
                    (reschedl & (RESCHED_SCHEDULE | RESCHED_THREAD))) {
                    if ((my_inkernel & INKERNEL_NOW) &&
                        (preempt_prio <= act->priority)) {
                        preempt_prio = act->priority;
                    }
                    atomic_fetch_or_explicit(&act->async_flags,
                                             RTMUX_ATF_TIMESLICE,
                                             memory_order_release);
                }
            }
        }
    }

    /* Global time update — only on boot CPU to avoid double-counting */
    if (my_cpu == boot_cpunum) {
        qtp->nsec += elapsed_nsec;
        MEM_BARRIER_WR();

        if (qtp->adjust.tick_count) {
            qtp->nsec_tod_adjust += (unsigned long) qtp->adjust.tick_nsec_inc;
            if (--qtp->adjust.tick_count == 0) {
                qtp->adjust.tick_nsec_inc = 0;
            }
            MEM_BARRIER_WR();
        }

        *nssptr = qtp->nsec;

        /* timer_expiry modifies global timer queue — skip if kernel is active
         * on any CPU. Besides protecting the wheel (now also covered by
         * timer_wheel_slock), this guard closes a block/timeout window: block()
         * and block_and_ready_default() arm a thread's timeout BEFORE taking
         * the scheduler lock and switching pc_curthread away, so firing that
         * timeout from this CPU's expiry while the target is still mid-kcall on
         * another CPU would ready() a thread that is concurrently blocking — a
         * cross-CPU corruption. Deferring expiry until no CPU is in-kernel
         * avoids it.
         */
        if (!(my_inkernel & INKERNEL_NOW)) {
            timer_expiry(qtp);

            /* timer_expiry() may have fired/recomputed the earliest deadline;
             * reprogram so dynamic-tick picks up the new target. The top-of-
             * handler reload remains the only rearm on the INKERNEL_NOW path
             * (which skips this block) and on non-boot CPUs.
             */
            (void) arch_timer_reload(qtp);
        }

#if defined(CONFIG_INSTRUMENTATION)
        /* Dump the interrupt-disabled window stats every 1 s. Reached only
         * outside a kcall (clock_handler bailed above on in_kernel_context), so
         * kprintf here is not nested inside a measured lock window. section 18.
         */
        {
            static uint64_t irqoff_last_report;
            if (qtp->nsec - irqoff_last_report >= 1000000000ULL) {
                irqoff_last_report = qtp->nsec;
                nonpreempt_report();

                /* Composite idle deadline, diagnostics only for now: show the
                 * winning owner plus every owner's own candidate (relative to
                 * now), so a trace can confirm each owner is a real participant
                 * in the minimum, not just the one that currently wins.
                 */
                unsigned owner;
                uint64_t cands[CLOCK_OWNER_STALL + 1];
                for (unsigned i = 0; i <= CLOCK_OWNER_STALL; i++)
                    cands[i] = ~(uint64_t) 0;
                uint64_t dl = clock_next_deadline(qtp, my_cpu, &owner, cands);
                if (dl != ~(uint64_t) 0)
                    kprintf(
                        "[deadline] owner=%u in=%lu ns\n", owner,
                        (unsigned long) (dl > qtp->nsec ? dl - qtp->nsec : 0));
                else
                    kprintf("[deadline] owner=none (tickless-eligible)\n");
                for (unsigned i = CLOCK_OWNER_TIMER; i <= CLOCK_OWNER_STALL;
                     i++)
                    if (cands[i] != ~(uint64_t) 0)
                        kprintf("[deadline]   cand owner=%u in=%lu ns\n", i,
                                (unsigned long) (cands[i] > qtp->nsec
                                                     ? cands[i] - qtp->nsec
                                                     : 0));

                /* Per-hart STIMER interrupts taken in the last reporting
                 * second. A quiet idle hart under tickless idle drops from the
                 * nsec_inc heartbeat rate (thousands/s) to only the composite
                 * deadlines it actually has, so the tickless-idle probe can
                 * read these lines to prove the periodic tick is gone.
                 */
                static uint64_t stimer_last[CONFIG_PROCESSORS_MAX];
                for (unsigned i = 0; i < num_processors; i++) {
                    uint64_t cur = atomic_load_explicit(&stimer_intr_count[i],
                                                        memory_order_relaxed);
                    kprintf("[tickless] cpu=%u stimer=%lu/s\n", i,
                            (unsigned long) (cur - stimer_last[i]));
                    stimer_last[i] = cur;
                }
            }
        }
#endif
    }

    if (preempt_prio > 0) {
        intrevent_preemption(preempt_prio + 1);
    }
    return NULL;
}


/* clock_next_deadline -- earliest absolute monotonic-ns deadline that idle
 * timer programming must not sleep past. It takes the minimum over every owner
 * that still needs a wakeup:
 *
 *   timer wheel     the earliest armed one-shot/periodic timer
 *   RR timeslice    the running RR/OTHER thread's remaining slice expiry
 *   sporadic server the next pending replenishment
 *   TOD adjustment  the next tick of an in-progress adjtime convergence
 *   stall detector  the heartbeat-silence watchdog window
 *
 * All candidates are expressed in the same monotonic since-boot nanosecond base
 * as arch_timer_now_ns(). The function only reads shared state (no locks, no
 * allocation), so it is safe from the idle path and from the clock ISR. It
 * returns all-ones when no owner needs a wakeup, meaning the hart may sleep
 * indefinitely. When owner is non-NULL it receives the winning CLOCK_OWNER_*
 * tag, for diagnostics only. When cands is non-NULL it is a caller-initialized
 * array indexed by CLOCK_OWNER_* that receives each evaluated owner's candidate
 * (owners not evaluated keep the caller's initial value), so a diagnostic can
 * show every owner's deadline, not just the winning one.
 */
/* Keep the earliest of *best and cand, tagging its owner. Folds the repeated
 * "if (cand < *best) { *best = cand; *who = tag; }" tail every candidate below
 * shares, so each owner stays a single line that spells out only its own
 * deadline computation. When cands is non-NULL it also records this owner's
 * candidate at cands[tag], so a diagnostic can show every owner's value, not
 * just the winner.
 */
static void deadline_min(uint64_t *best,
                         unsigned *who,
                         uint64_t cand,
                         unsigned tag,
                         uint64_t *cands)
{
    if (cands)
        cands[tag] = cand;
    if (cand < *best) {
        *best = cand;
        *who = tag;
    }
}

uint64_t rdecl clock_next_deadline(systime_t *qtp,
                                   unsigned cpu,
                                   unsigned *owner,
                                   uint64_t *cands)
{
    uint64_t best = ~(uint64_t) 0;
    unsigned who = CLOCK_OWNER_NONE;
    uint64_t now = arch_timer_now_ns(qtp);

    /* Timer wheel, sporadic server, TOD adjustment, and the stall detector are
     * all driven only by the boot CPU's clock_handler (timer_expiry,
     * sched_ss_adjust, the adjust.tick_count decrement, and stall_detector_tick
     * on qtp->nsec all gate on or advance from the boot hart). A non-boot hart
     * must not wake for them, so only the boot CPU contributes those.
     */
    if (cpu == boot_cpunum) {
        /* Timer wheel: cached earliest armed deadline (already monotonic ns).
         */
        uint64_t timer_dl;
        timer_next_deadline(&timer_dl);
        deadline_min(&best, &who, timer_dl, CLOCK_OWNER_TIMER, cands);

        /* Sporadic-server replenishment: head of the replenishment list. */
        if (ss_replenish_list)
            deadline_min(&best, &who, ss_replenish_time, CLOCK_OWNER_SS, cands);

        /* TOD adjustment: while an adjtime is converging it applies
         * tick_nsec_inc once per nsec_inc, so the boot CPU still needs a tick
         * one period out. adjust.tick_count is read unlocked here as a
         * conservative snapshot; a stale read costs at most one extra or one
         * deferred nsec_inc wake, which the diagnostic tolerates and the
         * software-time-publication work tightens before this drives idle.
         */
        if (qtp->adjust.tick_count && qtp->nsec_inc)
            deadline_min(&best, &who, now + qtp->nsec_inc, CLOCK_OWNER_TOD,
                         cands);

        /* Stall detector: heartbeat-silence watchdog window. */
        deadline_min(&best, &who, stall_next_deadline(), CLOCK_OWNER_STALL,
                     cands);
    }

    /* Round-robin is per-hart: this cpu's running RR/OTHER thread must be
     * preempted when its slice is spent. clock_handler charges rr_ticks with
     * round-half-up scaling, (elapsed * RR_FULLTICK + nsec_inc/2) / nsec_inc,
     * so a full nsec_inc period adds RR_FULLTICK ticks and the slice can trip
     * up to half a tick early. Use that earliest-possible expiry, (2*remain -
     * 1) * nsec_inc / 4, so idle never sleeps past a preemption.
     */
    {
        thread_t *act = get_active_thread(cpu);

        /* The idle thread is SCHED_RR at priority 0 and always STATE_RUNNING on
         * a quiet hart, but its timeslice never needs enforcing: it only runs
         * when nothing else is ready, so there is nothing to preempt it for.
         * Counting it here would arm an RR-slice tick every quiet slice and
         * defeat tickless idle, so exclude it.
         */
        if (act && act != pcpu_array[cpu].pc_idle_thp &&
            act->state == STATE_RUNNING && !kopt.nopreempt &&
            IS_SCHED_RR(act) && qtp->nsec_inc) {
            unsigned used = act->schedinfo.rr_ticks;
            uint64_t remain = used < RR_MAXTICKS ? RR_MAXTICKS - used : 0;
            uint64_t slice = remain ? (2 * remain - 1) * qtp->nsec_inc / 4 : 0;
            deadline_min(&best, &who, now + slice, CLOCK_OWNER_RR, cands);
        }
    }

    if (owner)
        *owner = who;
    return best;
}

/* Wake the clock back up after power management shut it down.
 *
 * No power-management idle hook is installed in RTMux today.
 */
void rdecl snap_time_wakeup(void) {}

/* Get the current time.
 *
 * If the current nsec == -1 then power management has shut down the clock
 * interrupt. In this case we invoke the power management callout to load the
 * current time from somewhere (rtc on a PC) and re-enable the clock interrupt.
 */
void rdecl snap_time(uint64_t *tsp, int incl_tod)
{
    systime_t *qtp = qtimeptr;
    uint64_t now, adjust;

    /* Read the monotonic base from arch_timer_now_ns, which tracks the
     * free-running cycle counter, rather than the published qtp->nsec. Under
     * tickless idle the boot CPU republishes qtp->nsec only when it wakes, so
     * qtp->nsec freezes across a skipped-tick gap while real time keeps moving;
     * a reader spinning on clock_gettime would otherwise see time stand still.
     * arch_timer_now_ns is continuous, and the timer wheel fires against the
     * qtp->nsec that clock_handler refreshes to this same value at each tick,
     * so relative sleeps stay consistent with these reads. Retry if a
     * concurrent clock_settime moves nsec_tod_adjust so REALTIME sees a
     * consistent pair.
     */
    do {
        adjust = qtp->nsec_tod_adjust;
        now = arch_timer_now_ns(qtp);
        if (now == (uint64_t) -1) {
            snap_time_wakeup();
        }
    } while (adjust != qtp->nsec_tod_adjust);
    *tsp = now;
    if (incl_tod) {
        *tsp += adjust;
    }
}

/* Compute (val * mul) / div using a 64-bit intermediate to avoid overflow.
 * Parameters:
 *   val : value to scale
 *   mul : multiplier
 *   div : divisor
 * Return value: the scaled result.
 */
static unsigned long muldiv(unsigned long val,
                            unsigned long mul,
                            unsigned long div)
{
    return ((uint64_t) val * mul) / div;
}

#define NANO_SCALE (-9)

/* Compute 10^factor. Parameters:
 *   factor : non-negative power of ten
 * Return value: 10 raised to factor.
 */
static unsigned long scale_factor(int factor)
{
    unsigned long scale = 1;

    while (factor != 0) {
        scale *= 10;
        --factor;
    }
    return scale;
}

/* Set the clock tick period.
 *
 * Converts the requested period in nanoseconds to a hardware timer load value,
 * programs the timer, and records the resulting nsec_inc. On hardware where the
 * timer lives on CPU 0 only, forwards the request via an IPI to CPU 0.
 * Parameters:
 *   nsec : requested tick period in nanoseconds
 */
void rdecl clock_resolution(unsigned long nsec)
{
    systime_t *qtp = qtimeptr;
    unsigned long scale;
    unsigned long timer_load;

    if ((qtp->flags & QTIME_FLAG_TIMER_ON_CPU0) && (get_cpunum() != 0)) {
        /* Force the load on CPU 0. Ends up invoking 'clock_load()' down below.
         */
        qtp->nsec_inc = nsec;
        SENDIPI(0, IPI_CLOCK_LOAD);
    } else {
        nsec += 1; /* Correct for rounding error on an exact specification. */

        if (qtp->timer_scale < NANO_SCALE) {
            scale = scale_factor((int) (NANO_SCALE - qtp->timer_scale));
            timer_load = muldiv(nsec, scale, qtp->timer_rate);
        } else {
            scale = scale_factor((int) (qtp->timer_scale - NANO_SCALE));
            timer_load = nsec / (qtp->timer_rate * scale);
            if (timer_load == 0)
                timer_load = 1;
        }
        qtp->timer_load = timer_load;
        if (qtp->timer_scale < NANO_SCALE) {
            nsec = muldiv(qtp->timer_load, qtp->timer_rate, scale);
        } else {
            nsec = qtp->timer_load * (qtp->timer_rate * scale);
        }
        qtp->nsec_inc = nsec;
        arch_timer_load(qtp);

        /* Disable any adjustment in effect. */
        INTR_LOCK(&clock_slock);
        qtp->adjust.tick_count = 0;
        qtp->adjust.tick_nsec_inc = 0;

        INTR_UNLOCK(&clock_slock);

        /* If the calculated tick interval has been rounded down to less than
         * what the user would be allowed to set in clock_period(), lower the
         * allowed minimum so that if the user does a clock_period(), saves the
         * clock tick, sets a new one and then restores to the old value, it all
         * works.
         */
        if (nsec < tick_minimum)
            tick_minimum = nsec;
    }
}

/* Reload the clock resolution on CPU 0.
 *
 * Used when the timer hardware is CPU-specific and a load must be forced on CPU
 * 0 via IPI_CLOCK_LOAD. The nsec_inc value was already set by a previous
 * clock_resolution() call.
 */
void rdecl clock_load()
{
    clock_resolution(qtimeptr->nsec_inc);
}

/* kext_clock_start — attach the clock handler and start the tick.
 *
 * Runs once at boot during roottask's clock-init. Single-threaded boot context
 * (other harts not yet dispatching), so the interrupt_vector +
 * interrupt_level[].queue mutations inside interrupt_attach are uncontested;
 * the in-kernel-call preemption fence is the per-CPU fence and no slock is
 * needed on this path. Parameters:
 *   data : pointer to the requested clock resolution (nsec)
 */
void kext_clock_start(void *data)
{
    int level;
    int id;

    level = get_interrupt_level(NULL, (unsigned) qtimeptr->intr);
    if (level < 0) {
        kprintf("Illegal clock intr\n");
        crash();
    }
    id =
        interrupt_attach((unsigned) level, &clock_handler, 0,
                         RTMUX_INTR_FLAGS_PROCESS | RTMUX_INTR_FLAGS_NO_UNMASK);
    if (id >= 0) {
        clock_isr = vector_lookup(&interrupt_vector, (unsigned) id);
        interrupt_level[level].mask_count = 0;
        SETKSTATUS(get_active_thread(get_cpunum()), 0);
        clock_resolution(*(unsigned long *) data);
        timer_period();
    }
}


/* Start the system clock.
 *
 * Selects the nsec_stable field if the syspage qtime entry is large enough,
 * then enters the kernel via __KSYS_PRIV_CLOCK_START to attach the handler and
 * begin ticking at the given resolution. Parameters:
 *   nsec : initial tick period in nanoseconds
 */
void clock_start(unsigned long nsec)
{
    /* Tell outside users that they should be looking at the nsec_stable field
     */
    if (_syspage_ptr->qtime.entry_size >=
        (offsetof(struct qtime_entry, nsec_stable) + sizeof(uint64_t))) {
        qtimeptr->flags |= QTIME_FLAG_CHECK_STABLE;
        nssptr = &qtimeptr->nsec_stable;
    } else {
        /* After a suitable period of time, we can assume (or crash if not) new
         * startups that have allocated the bigger struct qtime_entry. After
         * that, the nssptr variable can be removed and uses of it can be
         * replaced with qtimeptr->nsec_stable.
         */
    }

    /* RTMux-RISC-V startup never sets QTIME_FLAG_TIMER_ON_CPU0: the machine
     * timer is per-hart, so no CPU0 affinity is needed.
     */
    if (qtimeptr->flags & QTIME_FLAG_TIMER_ON_CPU0) {
        crash_halt(
            "clock_start: QTIME_FLAG_TIMER_ON_CPU0 set but no "
            "kernel-internal thread_ctl wrapper");
    }
    if (syscall_priv(__KSYS_PRIV_CLOCK_START, &nsec) < 0) {
        kprintf("clock intr failure (%d)\n", errno);
        crash();
    }
}
