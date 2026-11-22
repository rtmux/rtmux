/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

/* Scheduler Rosetta Stone (comments/prose only; no symbol renames):
 *
 *   kdispatch_t              = per-CPU runqueue state
 *   ready[priority]        = priority-indexed runnable queue
 *   SENDIPI(cpu, IPI_RESCHED) = cross-CPU reschedule IPI
 *   runmask                = excluded-CPU mask; set_runmask() stores ~mask, so
 *                            a set bit means "do not run me on this CPU"
 *   runcpu                 = last CPU placement
 *   block()/unready()      = remove current thread from runnable state
 *   ready()                = enqueue a runnable thread
 *
 * RTMux keeps historical identifiers. Linux-familiar terms appear here so
 * readers can map the scheduler model once, then read the native names below.
 */

#ifndef NDEBUG

static void rdecl ready_default(thread_t *thp);

static void chk_dispatch(int chkstate)
{
    unsigned int i;
    thread_t *thp;
    LINK3_NODE **owner;
    kdispatch_t *dpp;
    thread_t *act;

    if (kopt.loglevel < 3) {
        return;
    }
    if (ready_default != ready)
        return;

    act = get_active_thread(get_cpunum());
    if (!act)
        return; /* idle CPU — no active thread to validate */
    if (!act->process)
        crash();
    if (!act->dpp)
        crash();
    if (chkstate && act->state != STATE_RUNNING)
        crash();

    /* chk_dispatch is retired as a runtime check under per-CPU sched locks.
     * Most callers invoke it BEFORE taking the local lock, and even when our
     * lock IS held, the lock bit test may instead mean "held by ANOTHER
     * CPU doing a cross-CPU enqueue/steal on our queue" — legitimate under
     * per-CPU locks. Distinguishing "I hold it" from "someone else holds it"
     * would need a per-CPU sched_held_by_me[] flag, which is not maintained.
     * Since the check is debug-only (gated on kopt.loglevel >= 3 above), the
     * lost validation is acceptable; re-enable via a held-by-me flag if a
     * future bug needs it. Walking other harts' queues here would also observe
     * the transient (bit-set, head-NULL) window between LINK3_REM and
     * DISPATCH_CLR and fire the invariant incorrectly.
     */
    return;
    (void) dpp;
    for (i = 0; i < NUM_PRI; ++i) {
        if (DISPATCH_ISSET(dpp, i)) {
            owner = &DISPATCH_LST(dpp, i).head;
            if (!*owner) {
                /* Transient: bitmap set, queue empty — benign race with
                 * concurrent LINK3_REM/DISPATCH_CLR on same CPU when
                 * chk_dispatch ran without holding the local sched lock.
                 */
                continue;
            }

            for (;;) {
                thp = link3_to_thread(*owner);
                if (!thp)
                    break;
                if (!thp->process) {
                    crash();
                }
                if (thp->priority != i) {
                    crash();
                }
                if (thp->state != STATE_READY && (chkstate || thp != act)) {
                    crash();
                }
                if (TYPE_MASK(thp->type) != TYPE_THREAD) {
                    crash();
                }
                if (thp->link3_link.prev != owner) {
                    crash();
                }

                /* No "thp->dpp == dpp" check here: chk_dispatch's pre-lock
                 * callers race against another CPU's cross-CPU enqueue that
                 * publishes thp on this queue with thp->dpp = dpp. Without
                 * acquire-ordered reads the membership and the dpp pointer are
                 * not observed atomically; the invariant still holds under the
                 * lock at the moment of mutation.
                 */
                owner = &thp->link3_link.next;
            }
        } else {
            if (DISPATCH_THP(dpp, i)) {
                /* Transient inverse: queue head non-null while bitmap not yet
                 * set (mid-LINK3_BEG).
                 */
                continue;
            }
        }
    }
}

#else

#define chk_dispatch(chkstate)

#endif

#define chk_thread(thp) CRASHCHECK(TYPE_MASK((thp)->type) != TYPE_THREAD)

static void rdecl mark_running_default(thread_t *new);

/* Bill this hart's active thread up to now (see sched_account_current).
 *
 * snap_time() is arch_timer_now_ns(), free-running at ns resolution, because
 * RISC-V arch-init always sets qtp->timer_scale = -9. A port with another
 * timer_scale would get tick-granular qtp->nsec here and billing would degrade
 * to tick resolution (still correct in total, never double counted).
 */
static inline void sched_account_now(void)
{
    uint64_t now;

    snap_time(&now, 0);
    sched_account_current(now);
}

/* Bill the active thread before it is enqueued STATE_READY and the dispatch
 * lock dropped: once act is on a ready queue another (idle) hart may steal and
 * run it, after which this hart must not write its running_time. The following
 * mark_running() sees pc_account_billed and only restarts the stamp.
 */
static inline void sched_account_expose(void)
{
    sched_account_now();
    get_pcpu()->pc_account_billed = 1;
}

#if CONFIG_PROCESSORS_MAX > 1 /* PDB condition is true */

#define VALIDATE_NEEDTORUN(thp)                                    \
    if (need_to_run &&                                             \
        (need_to_run_cpu == get_cpunum() || need_to_run == thp)) { \
        need_to_run = NULL;                                        \
    }

static int dispatch_unlink_ready(kdispatch_t *dpp, thread_t *thp)
{
    LINK3_HDR *queue = &DISPATCH_LST(dpp, thp->priority);

    if (!thp->link3_link.prev) {
        if (queue->head != &thp->link3_link)
            return 0;
        thp->link3_link.prev = &queue->head;
    }

    LINK3_REM(*queue, &thp->link3_link, thread_t);
    THR_EV(thp);
    if (!DISPATCH_THP(dpp, thp->priority))
        DISPATCH_CLR(dpp, thp);
    return 1;
}

/* Wake one idle hart that thp may run on, so its idle_pick_thread steals thp
 * from the busy hart it was queued on. No-op when every legal hart is busy.
 *
 * A sleeping hart's active slot names its idle thread (cpu_idle points it there
 * so pc_curthread never lags), not NULL; NULL is a hart not yet started.
 * Testing for NULL alone never matched a sleeping hart, so the kick never fired
 * and a thread queued behind a busy one waited for an idle hart's next timer
 * interrupt, which under tickless idle can be seconds away.
 */
static void kick_idle_hart(thread_t *thp)
{
    unsigned mycpu = get_cpunum();
    uint64_t legal = __atomic_load_n(&LEGAL_CPU_BITMASK, __ATOMIC_RELAXED);
    uint64_t live = legal & cpu_online_snapshot();

    for (unsigned h = 0; h < num_processors; h++) {
        thread_t *act;

        /* A dropped CPU keeps the idle thread init_objects() gave it, so it
         * would look asleep and absorb the kick; skip it, and a CPU that has
         * not joined yet, which cannot take the interrupt.
         */
        if (!cpu_in_mask(live, h) || h == mycpu ||
            (thp->runmask & (1u << h)) != 0)
            continue;
        act = get_active_thread(h);
        if (act && act == pcpu_array[h].pc_idle_thp) {
            SENDIPI(h, IPI_RESCHED);
            return;
        }
    }
}

static int rdecl select_cpu(thread_t *thp)
{
    thread_t *act;
    unsigned i, j = 0;
    int cpu = -1;
    unsigned minprio;
    unsigned alternates[PROCESSORS_MAX];
    uint64_t legal = __atomic_load_n(&LEGAL_CPU_BITMASK, __ATOMIC_RELAXED);

    {
        for (int _hi = (int) num_processors - 1, _init = 1; _hi >= 0; --_hi) {
            i = (unsigned) _hi;
            if (_init) {
                j = 0;
                cpu = -1;
                minprio = thp->priority;
                _init = 0;
            }
            if (!smp_dispatch_enabled && i != get_cpunum())
                continue;
            if (!cpu_in_mask(legal, i))
                continue; /* offline: its hart never started */
            if ((thp->runmask & (1u << i)) == 0) {
                act = get_active_thread(i);
                if (!act)
                    continue; /* CPU not yet running a thread */
                if (act->priority < minprio) {
                    j = 0;
                    cpu = (int) i;
                    minprio = act->priority;
                } else if (act->priority == minprio) {
                    alternates[j++] = i;
                }
            }
        }
    }

    /* If we found a cpu make sure it is the best one in the case of multiple
     * choices!
     */
    if (cpu != -1 && thp->runcpu != (unsigned) cpu && j) {
        while (j) {
            unsigned new_cpu;

            new_cpu = alternates[--j];
            if (new_cpu == thp->runcpu) {
                cpu = (int) new_cpu;
            }
        }
    }

    return cpu;
}

/**
 * Select_thread function for SMP. Given cpu and prio, returns a runnable thread
 * with priority equal to or better than prio.
 */
static thread_t *rdecl select_thread_default(int cpu, int prio)
{
    thread_t *thp;
    uint32_t runmask;
    int hipri;
    kdispatch_t *dpp;

    runmask = 1u << cpu;

    /* Scan THIS CPU's per-CPU dispatch first. If nothing runnable here, fall
     * through to a steal scan over every other hart's dispatch. The caller
     * holds the local dispatch lock; the steal scan takes each remote
     * dispatch's lock independently.
     */
    dpp = pcpu_array[cpu].pc_dispatch;
    hipri = DISPATCH_HIGHEST_PRI(dpp);
    for (; hipri >= prio && hipri > -1; hipri--) {
        for (thp = DISPATCH_THP(dpp, hipri); thp;
             thp = link3_to_thread(thp->link3_link.next)) {
            if ((thp->runmask & runmask) == 0) {
                if ((int) (unsigned) thp->priority >= prio) {
                    if (!dispatch_unlink_ready(dpp, thp))
                        continue;
                }
                return thp;
            }
        }
    }

    /* Steal phase: caller holds LOCAL_DPP. Drop it so we can take remote dpp
     * locks without ABBA against other harts running the symmetric scan. For
     * each remote, lock-scan-steal-unlock independently; re-acquire LOCAL_DPP
     * before returning (caller expects it).
     *
     * Race window: between drop and re-acquire on this CPU, something may
     * enqueue locally. That's benign — caller is about to dispatch whatever we
     * return. If we don't find anything to steal, the function returns NULL and
     * the caller proceeds (it'll observe the local enqueue on its own next
     * cycle if there is one).
     */
    {
        SCHED_UNLOCK(pcpu_array[cpu].pc_dispatch);

        for (int idx = 0; idx < (int) num_processors; idx++) {
            unsigned other = (unsigned) idx;

            /* Not cpu_online(): a thread queued on an AP before it joined or
             * was dropped is only ever taken from its queue here.
             */
            if (other == (unsigned) cpu)
                continue;
            kdispatch_t *odpp = pcpu_array[other].pc_dispatch;
            if (!odpp)
                continue;
            SCHED_LOCK(odpp);
            int ohi = DISPATCH_HIGHEST_PRI(odpp);
            if (ohi < prio || ohi < 0) {
                SCHED_UNLOCK(odpp);
                continue;
            }
            for (thp = DISPATCH_THP(odpp, ohi); thp;
                 thp = link3_to_thread(thp->link3_link.next)) {
                if ((thp->runmask & runmask) == 0 &&
                    (int) (unsigned) thp->priority >= prio) {
                    if (!dispatch_unlink_ready(odpp, thp))
                        continue;
                    thp->dpp = pcpu_array[cpu].pc_dispatch;
                    SCHED_UNLOCK(odpp);
                    SCHED_LOCK(pcpu_array[cpu].pc_dispatch);
                    return thp;
                }
            }
            SCHED_UNLOCK(odpp);
        }

        SCHED_LOCK(pcpu_array[cpu].pc_dispatch);
    }

    return NULL;
}

#define FIND_HIGHEST(act)                          \
    (act) = select_thread((int) get_cpunum(), -1); \
    if (act)                                       \
    (act)->runcpu = get_cpunum()

/* Wait for the preemptible lockless ss_info readers to finish with thp.
 *
 * Half of a Dekker handshake with resched_default() and ksys_sched_get() — the
 * readers of thp->schedinfo.ss_info that run with interrupts enabled and can
 * therefore pass an RCU-sched quiescent state mid-access, so the grace period
 * in sched_ss_cleanup() alone would not cover them (clock_handler and the
 * SCHED_LOCK-held scheduler-core readers are non-preemptible and are covered by
 * that grace period instead). The caller has already stored a non-SPORADIC
 * policy into thp, so a fresh reader won't enter the ss_info block; the leading
 * fence orders that store ahead of the ticker_using load, and each reader bumps
 * ticker_using behind a matching fence before it reads the policy. So either we
 * observe a nonzero reader count and spin until every reader clears it, or
 * those readers observe the new policy and skip the deref — the ss_info free
 * that follows can never land mid-access. ticker_using is a count (a reader can
 * nest a timer tick), so load it atomically.
 */
#define WAIT_FOR_CLOCK_DONE(thp)                                        \
    do {                                                                \
        __cpu_membarrier();                                             \
        while (__atomic_load_n(&(thp)->ticker_using, __ATOMIC_RELAXED)) \
            __cpu_membarrier();                                         \
    } while (0)

#else /* uniprocessor */
#define VALIDATE_NEEDTORUN(thp)

#define FIND_HIGHEST(act) ((act) = select_thread(0, -1))

/**
 * Select_thread function for UP. Given prio, returns a runnable thread with
 * priority equal to or better than prio. cpu is ignored in the case of
 * single-cpu.
 */
static thread_t *rdecl select_thread_default(int cpu, int prio)
{
    thread_t *thp;
    kdispatch_t *dpp;
    int hipri;
    (void) cpu;

    /* UP path uses pcpu_array[my_cpu].pc_dispatch (the only active hart's
     * instance).
     */
    dpp = pcpu_array[get_cpunum()].pc_dispatch;

    hipri = DISPATCH_HIGHEST_PRI(dpp);
    if (hipri < prio)
        return NULL;

    thp = DISPATCH_THP(dpp, hipri);
    if (!thp) {
        /* May occur if there is nothing, not even idle */
        return NULL;
    }

    LINK3_REM(DISPATCH_LST(dpp, (thp)->priority), &(thp)->link3_link, thread_t);
    THR_EV(thp);

    /* If that list is now empty, clear the readybit.   */
    if (!DISPATCH_THP(dpp, (thp)->priority)) {
        DISPATCH_CLR(dpp, (thp));
    }

    return thp;
}

/* wait for clock handler to finish with thread - nothing to do for uni */
#define WAIT_FOR_CLOCK_DONE(thp)

#endif

/**
 * Mark a thread as the one being in execution on this CPU
 */
static void rdecl mark_running_default(thread_t *new)
{
    thread_t *old;
    int was_intr;

    old = get_active_thread(get_cpunum());
    was_intr = new->state == STATE_INTR;
    THR_EV(new);
    if (!new->process) {
        thr_trace_dump(new);
        kprintf(
            "FATAL: mark_running cpu=%u new=%p tid=%d process=NULL type=%u "
            "state=%u\n",
            get_cpunum(), (void *) new, new->tid, new->type, new->state);
        crash();
    }

    /* Switch-time billing: charge old for its stretch and start new's. Must
     * precede rcu_sched_quiescent() (the sporadic ss_info deref in
     * sched_account_current is only protected until this hart reports a
     * quiescent state) and set_active_thread() (so the stamp already belongs to
     * new when a remote seqcount reader first sees new as this hart's active
     * thread). If old was billed earlier, before being enqueued READY, the flag
     * makes this call only restart the stamp.
     */
    sched_account_now();
    if (old != new)
        rcu_sched_quiescent();
    set_active_thread(get_cpunum(), new);

    new->state = STATE_RUNNING;
    new->runcpu = get_cpunum();

    /* While RUNNING, thp->dpp must point at this CPU's per-CPU dispatch
     * instance. Defensive snap for the rare path where a thread reached RUNNING
     * without an intervening ready_default that already routed dpp (e.g.
     * initial dispatch on a non-boot CPU, or a steal from idle_pick_thread
     * which sets dpp explicitly — redundant here, no harm).
     */
    new->dpp = pcpu_array[get_cpunum()].pc_dispatch;

    _TRACE_TH_EMIT_STATE(new, RUNNING);
    if (was_intr) {
        _TRACE_IST_DISPATCH(NULL, new);
    }

    VALIDATE_NEEDTORUN(new)

    /* No WAIT_FOR_CLOCK_DONE(old) here. That handshake belongs to
     * sched_ss_cleanup(), which calls it itself before freeing ss_info. Waiting
     * on old's ticker_using from the switch path self-deadlocks when old is the
     * very thread whose resched_default() bracket (around SS_CHECK_EXPIRY ->
     * sched_ss_update -> adjust_priority -> mark_running) holds that count: a
     * sporadic thread that spent its budget wedged its hart with the dispatch
     * lock held.
     */
}



/* CPU-time billing ***
 *
 * SMP invariant (Standing Gate): the active thread's running_time (and the
 * sporadic ss_info consumed/curr_budget it drives) is written ONLY by the hart
 * that owns it, i.e. the hart whose pc_curthread it is, and only from
 * sched_account_current() on that hart; no cross-hart writer exists. The owning
 * process's running_time is shared by that process's threads on every hart, so
 * it is updated with atomic_fetch_add. pc_account_stamp is likewise owner-hart
 * written; pc_account_seq is its seqcount (odd while an update is in flight) so
 * a remote clock read can add the live, not-yet-billed stretch to a running
 * thread without taking a lock (sched_account_live). Idle time and time spent
 * on a hart with no active thread are billed to nobody.
 *
 * Every path that changes this hart's active thread calls
 * sched_account_current() (thread->thread via mark_running_default, thread->
 * idle via block/block_release/yield-to-none) or sched_account_restart()
 * (idle->thread via cpu_idle) BEFORE set_active_thread(), so the outgoing
 * thread is charged exactly its own stretch. The tick also bills, so RLIMIT_CPU
 * and sporadic budgets are enforced against live consumption. A thread being
 * destroyed on its own hart is billed on its way off the CPU (unready ->
 * block); a dead or reaped pc_curthread that is abandoned in kernel_exit is not
 * billed, since its process may already be gone.
 */
void rdecl sched_account_current(uint64_t now)
{
    struct pcpu *pc = get_pcpu();
    thread_t *act = pc->pc_curthread;
    uint64_t stamp = pc->pc_account_stamp;
    uint64_t delta = now > stamp ? now - stamp : 0;
    unsigned seq =
        atomic_load_explicit(&pc->pc_account_seq, memory_order_relaxed);

    atomic_store_explicit(&pc->pc_account_seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);

    /* act was already billed and enqueued (sched_account_expose); it may be
     * running on another hart now, so it is not ours to write.
     */
    if (pc->pc_account_billed) {
        pc->pc_account_billed = 0;
        act = NULL;
    }

    if (delta && act && act != pc->pc_idle_thp && act->process) {
        act->running_time += delta;
        __atomic_fetch_add(&act->process->running_time, delta,
                           __ATOMIC_RELAXED);

        if (act->policy == SCHED_SPORADIC) {
            /* Reading ss_info is safe because every caller is a non-preemptible
             * RCU-sched reader that has NOT yet reported a quiescent state
             * (callers bill before rcu_sched_quiescent(); the tick runs in
             * interrupt context): sched_ss_cleanup() sets policy to FIFO and
             * then rcu_sched_synchronize()s before freeing ss_info.
             *
             * consumed/curr_budget have other writers on any hart
             * (sched_ss_adjust refill, sched_ss_queue/sched_thread resets), so
             * the owner-hart billing uses atomic RMW and never a stale
             * load-modify-store: a concurrent refill/reset store cannot be
             * overwritten. A bill racing a refill may charge the refilled
             * period for at most one delta.
             */
            sched_info_t *ss = act->schedinfo.ss_info;
            if (ss &&
                __atomic_load_n(&ss->org_priority, __ATOMIC_RELAXED) == 0) {
                uint64_t c =
                    __atomic_load_n(&ss->curr_budget, __ATOMIC_RELAXED);

                __atomic_fetch_add(&ss->consumed, delta, __ATOMIC_RELAXED);
                while (!__atomic_compare_exchange_n(
                    &ss->curr_budget, &c, c > delta ? c - delta : 0, 0,
                    __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                    ;
            }
        }
    }
    if (now > stamp)
        pc->pc_account_stamp = now;

    atomic_store_explicit(&pc->pc_account_seq, seq + 2, memory_order_release);
}

/* A thread is about to start on this hart from idle (or the hart has just come
 * up): begin its stretch now without billing the idle time.
 */
void rdecl sched_account_restart(void)
{
    struct pcpu *pc = get_pcpu();
    uint64_t now;
    unsigned seq =
        atomic_load_explicit(&pc->pc_account_seq, memory_order_relaxed);

    snap_time(&now, 0);
    atomic_store_explicit(&pc->pc_account_seq, seq + 1, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    pc->pc_account_stamp = now;
    atomic_store_explicit(&pc->pc_account_seq, seq + 2, memory_order_release);
}

/* Return thp's running_time including, when thp is running on some hart, the
 * not-yet-billed ns of its current stretch. The banked value and the stamp are
 * sampled under the owner hart's pc_account_seq seqcount, so they are a
 * consistent pair. After three torn attempts return the banked value alone. A
 * caller on thp's own hart should bill with sched_account_current() first.
 */
uint64_t rdecl sched_account_read(thread_t *thp, uint64_t now)
{
    for (int tries = 0; tries < 3; tries++) {
        unsigned cpu = __atomic_load_n(&thp->runcpu, __ATOMIC_ACQUIRE);
        if (!cpu_online(cpu))
            break;
        struct pcpu *pc = &pcpu_array[cpu];
        unsigned seq = atomic_load_explicit(&pc->pc_account_seq, memory_order_acquire);
        uint64_t stamp = pc->pc_account_stamp;
        uint64_t banked = thp->running_time;
        thread_t *cur = __atomic_load_n(&pc->pc_curthread, __ATOMIC_ACQUIRE);

        atomic_thread_fence(memory_order_acquire);
        if (!(seq & 1u) && cur == thp &&
            cpu == __atomic_load_n(&thp->runcpu, __ATOMIC_RELAXED) &&
            seq == atomic_load_explicit(&pc->pc_account_seq, memory_order_relaxed))
            return banked + (now > stamp ? now - stamp : 0);
        if (cur != thp && cpu == __atomic_load_n(&thp->runcpu, __ATOMIC_RELAXED))
            break;
    }
    return thp->running_time;
}

/*** Sporadic scheduler policy functions ***/
/**
 * This code is used to update the replenishment object(s) adding them to the
 * list if need be. Due to the way that execution is currently measured (via
 * timer tick), it is possible to call into this function with no consumed
 * execution. Since we want to do the replenishment bookkeeping here, simulate a
 * small amount of execution to ensure that we do schedule a replenishment.
 *
 * We have chosen to deviate slightly from POSIX's "by the book" algorithm where
 * a replenishment is scheduled for each time that a thread blocks. On a ukernel
 * system this leads to excessive fragmentation of the execution budget and a
 * lot of time bookkeeping. Instead we choose to schedule one replenishment and
 * then add to it as long as it is pending. Then we give back the whole amount
 * later on.
 *
 * @@@ What happens when the period is already passed .. skip this?
 */
void rdecl sched_ss_update(thread_t *thp,
                           sched_info_t *ssinfo,
                           sched_replenish_t *ssrepl,
                           int drop)
{
    sched_replenish_t **pssrepl;

    /* We don't do replenishments in low priority mode */
    if (ssinfo->org_priority != 0) {
        return;
    }

    if (ssrepl->amount == 0) { /* First replenishment in a period */
        ssrepl->amount = ssinfo->consumed;
        if (ssrepl->amount == 0) {
            ssrepl->amount++;
        }
        ssrepl->repl_time = ssinfo->activation_time + ssinfo->repl_period;

        pssrepl = &ss_replenish_list;
        while (*pssrepl && (*pssrepl)->repl_time < ssrepl->repl_time) {
            pssrepl = &(*pssrepl)->next;
        }

        /* Set the time before we set the head of the list */
        if (pssrepl == &ss_replenish_list) {
            ss_replenish_time = ssrepl->repl_time;
        }

        ssrepl->next = *pssrepl;
        *pssrepl = ssrepl;
    } else { /* Replenishment already scheduled */
        ssrepl->amount += ssinfo->consumed;
    }

    ssinfo->repl_count++;

    CRASHCHECK(ssrepl->amount == 0);

    if (drop) {
        unsigned target;

        /* The drop lowers the base only: a loan (PI waiter, ceiling, message)
         * above the low priority keeps holding the thread up.
         */
        ssinfo->org_priority = (uint8_t) thread_base_prio(
            thp); /* Guarantee no more ss operations */
        thread_set_base_prio(thp, ssinfo->low_priority);
        if (ssinfo->sched_locked) {
            /* thp is already blocked, and the caller holds the dispatch lock
             * (see SS_STOP_RUNNING_LOCKED): store it; the next loan or base
             * change on thp reapplies it under sync_pi_slock.
             */
            target = thread_effective_prio(thp);
            thp->priority = (uint8_t) target;
        } else {
            (void) thread_prio_update_dpp(thp, thp->dpp, 0); /* CHECKME */
        }
    }
}

/**
 * We run through the pending replenishment list to take care of business if
 * there are any pending replenishments which need to be cleaned out before we
 * actually re-schedule. This function returns 0 if we have not made any
 * adjustments to the priority.
 */
int rdecl sched_ss_adjust()
{
    int adjusted = 0;

    if (ss_replenish_list) {
        uint64_t bound;
        thread_t *thp;
        sched_replenish_t *ssrepl, **pssrepl;

        snap_time(&bound, 0);

        pssrepl = &ss_replenish_list;
        while ((ssrepl = *pssrepl) && (*pssrepl)->repl_time <= bound) {
            CRASHCHECK(!ssrepl->thp);
            CRASHCHECK(ssrepl->thp->policy != SCHED_SPORADIC);
            CRASHCHECK(!ssrepl->thp->schedinfo.ss_info);
            /* Set the time before me move the head */
            if (ssrepl->next) {
                ss_replenish_time = ssrepl->next->repl_time;
            }

            /* Remove this item from the replenishment list */
            *pssrepl = ssrepl->next;

            if ((thp = ssrepl->thp) && IS_SCHED_SS(thp)) {
                sched_info_t *ssinfo = thp->schedinfo.ss_info;

                /* Since we gather all replenishments together, always give back
                 * the entire budget. This will change if we do proper, per
                 * block, replenishment scheduling.
                 */
                ssinfo->curr_budget = ssinfo->init_budget;

                /* Give back all of the replenishments */
                ssinfo->repl_count = 0;

                /* If the priority has been reduced, boost back to original */
                if (ssinfo->org_priority != 0) {
                    ssinfo->activation_time = bound;
                    ssinfo->consumed = 0;

                    thread_set_base_prio(thp, ssinfo->org_priority);
                    (void) thread_prio_update_dpp(thp, thp->dpp,
                                                  0); /* CHECKME */

                    ssinfo->org_priority = 0;

                    adjusted++;
                }

                /* Clear the "first replenishment" indicator */
                ssrepl->amount = ssrepl->repl_time = 0;
            }
#ifndef NDEBUG
            else {
                crash();
            }
#endif
        }
    }

    chk_dispatch(1);
    return adjusted;
}

/**
 * This function will go through and clean-up after all of the SCHED_SPORADIC
 * pieces which might be floating around the system.
 */
void rdecl sched_ss_cleanup(thread_t *thp)
{
    sched_replenish_t **pssrepl, *ssrepl;
    sched_info_t *ssinfo = thp->schedinfo.ss_info;
#ifndef NDEBUG
    int found = 0;
#endif

    /* We're getting rid of the sporadic scheduling structure, so we have switch
     * to a policy that doesn't need it or else the clock interrupt might look
     * at some data that isn't there anymore. This function gets called from two
     * places: thread_destroy() so the policy doesn't matter anymore, or
     * sched_thread() which is going to whack in an updated policy very shortly.
     */
    thp->policy = SCHED_FIFO;

    WAIT_FOR_CLOCK_DONE(thp);

    pssrepl = &ss_replenish_list;
    while ((ssrepl = *pssrepl)) {
        if (ssrepl->thp == thp) {
            if (pssrepl == &ss_replenish_list && ssrepl->next) {
                ss_replenish_time = ssrepl->next->repl_time;
            }
            *pssrepl = ssrepl->next;
            /* Only one replenishment per thread */
#ifndef NDEBUG
            found++;
#else
            break;
#endif
        } else {
            pssrepl = &ssrepl->next;
        }
    }

    CRASHCHECK(found > 1);
    CRASHCHECK(ssinfo->replenishment.amount != 0 && found == 0);

    /* Drain any hart still reading ssinfo before freeing it. The readers split
     * by whether they can pass an RCU-sched quiescent state mid-access:
     *
     *   - Preemptible lockless readers (resched_default, ksys_sched_get) run
     *     with interrupts on and could context-switch between the policy test
     *     and the ss_info deref, so a grace period alone would not cover them.
     *     WAIT_FOR_CLOCK_DONE above waits them out via the ticker_using
     *     handshake instead.
     *   - Non-preemptible readers — clock_handler (interrupt context) and the
     *     scheduler-core SS_STOP_RUNNING / SS_MARK_ACTIVATION / SS_CHECK_EXPIRY
     *     paths (all under SCHED_LOCK with IRQs off, or gated behind a non-
     *     SPORADIC policy so they never deref) — cannot pass a quiescent state
     *     mid-access, so they are exactly what RCU-sched covers. policy was set
     *     to SCHED_FIFO above so no new reader enters; the grace period then
     *     waits out the in-flight ones (each quiesces on interrupt return /
     *     context switch) before we NULL and free. This is what the migration-
     *     loose per-CPU dispatch lock cannot be relied on to guarantee for a
     *     foreign caller that holds no dispatch lock.
     *
     * Legal here because every caller (sched_thread, thread_specret,
     * thread_destroy) is lock-free — rcu_sched_synchronize() crashes if a
     * kspinlock is held. Cost: a sched_setscheduler() leaving SCHED_SPORADIC
     * now blocks one grace period; it is a rare, non-realtime transition, but
     * worth an RT-budget row if that path ever gets latency-sensitive.
     */
    rcu_sched_synchronize();

    thp->schedinfo.ss_info = NULL;
    object_free(thp->process, &ssinfo_souls, ssinfo);
}

/**
 * This function is called when this thread transitions from RUNNING to
 * something else (presumably blocking!) so that we can cause a replenishment
 * operation to happen.
 *
 * This assumes that the state of the thread which was running has already been
 * set. If the state is READY then we have been pre-empted and nothing has to
 * occur.
 */
void rdecl sched_ss_block(thread_t *thp, int preempted)
{
    CRASHCHECK(thp->policy != SCHED_SPORADIC);
    if (thp->schedinfo.ss_info->org_priority != 0) {
        return;
    }

    /* TODO: Do the execution snapshot here and check for expiry Only run this
     * code if we are being blocked (not pre-empted)
     */
    if (preempted) {
        return;
    }

    /* Nothing consumed since activation: the thread blocked without using any
     * of its budget, so there is nothing to replenish. Dropping here anyway
     * (sched_ss_update forces a phantom amount of 1 when consumed == 0) queues
     * a replenishment and lowers the thread to ss_low_priority; if it then goes
     * idle waiting on that block, the replenishment is its only path back to
     * high priority — and a thread that used no budget should never have needed
     * one. Leave it at its normal priority instead.
     */
    if (thp->schedinfo.ss_info->consumed == 0) {
        return;
    }

    /* This will increment the replenishment count. The rule is that we stay
     * high if we are strictly lower than the replenishment count. This accounts
     * for the +1
     */
    sched_ss_update(thp, thp->schedinfo.ss_info,
                    &thp->schedinfo.ss_info->replenishment,
                    ((thp->schedinfo.ss_info->repl_count + 1) >=
                     thp->schedinfo.ss_info->max_repl));
}

/* Start a new sporadic activation for thp, unless it runs boosted. */
static void sched_ss_activate(thread_t *thp)
{
    if (thp->schedinfo.ss_info->org_priority != 0)
        return;
    snap_time(&thp->schedinfo.ss_info->activation_time, 0);
    thp->schedinfo.ss_info->consumed = 0;
}

/**
 * This function is called when we put a thread at the tail of the READY queue
 * and we need to reset the activation time and clear the amount of execution
 * time it has used.
 *
 * This assumes that the state of the thread is _going_ to be set to READY and
 * that if the thread is being boosted that it has already cleared the dropped
 * flag.
 */
void rdecl sched_ss_queue(thread_t *thp)
{
    CRASHCHECK(thp->policy != SCHED_SPORADIC);

    switch (thp->state) {
    case STATE_RUNNING: /* Time already snapped for these guys */
    case STATE_READY:
        break;

    default:
        sched_ss_activate(thp);
        break;
    }
}

/*** End of Sporadic scheduler attributes ***/

/**
 * This function is called from thread_specret() and ksys_sched_set()
 */
int rdecl sched_thread(thread_t *thp, int policy, struct sched_param *param)
{
    unsigned new_prio;
    unsigned old_prio;
    kdispatch_t *dpp;
    int verify;
    struct _ss_schedinfo *ss_info;

    switch (policy) {
    case RTMUX_SCHED_ADJTOHEAD:
    case RTMUX_SCHED_ADJTOTAIL:
        /* Just a positional change, not a parameter change. */
        dpp = thp->dpp;

        if (thp->state != STATE_READY) {
            return EINVAL;
        }

        LINK3_REM(DISPATCH_LST(dpp, thp->priority), &thp->link3_link, thread_t);
        if (policy == RTMUX_SCHED_ADJTOHEAD) {
            LINK3_BEG(DISPATCH_LST(dpp, thp->priority), &thp->link3_link,
                      thread_t);
        } else {
            RR_RESET_TICK(thp);
            LINK3_END(DISPATCH_LST(dpp, thp->priority), &thp->link3_link,
                      thread_t);
        }

        return EOK;
    case RTMUX_SCHED_NOCHANGE:
        policy = thp->policy;
        break;
    case RTMUX_SCHED_SETPRIO:
        if (param->sched_priority <= 0 || param->sched_priority >= NUM_PRI) {
            return EINVAL;
        } else {
            new_prio = (unsigned) param->sched_priority;
        }
        if (!(thp->process->priv_caps & RTMUX_PRIV_CAP_SCHED) &&
            new_prio >= kopt.priv_prio) {
            return EPERM;
        }
        if (thp->policy == SCHED_SPORADIC &&
            thp->schedinfo.ss_info->org_priority != 0) {
            thp->schedinfo.ss_info->org_priority = (uint8_t) new_prio;
        }

        /* SchedSet sets the base and ends a notification loan. A loan above it
         * (a PI waiter, a ceiling, a request being served, a send still queued)
         * keeps the thread at the loan until it ends.
         */
        old_prio = thp->priority;
        thread_end_notification_loan(thp);
        thread_set_base_prio(thp, new_prio);
        (void) thread_prio_update_dpp(thp, thp->dpp, 0);

        /* If we lowered priority, ensure we will be head of priority list */
        if (thp->priority < old_prio && thp->state == STATE_READY) {
            LINK3_REM(DISPATCH_LST(thp->dpp, thp->priority), &thp->link3_link,
                      thread_t);
            LINK3_BEG(DISPATCH_LST(thp->dpp, thp->priority), &thp->link3_link,
                      thread_t);
        }
        return EOK;
    default:
        break;
    }
    new_prio = (unsigned) param->sched_priority;

    /* Verify the policy and priority (and sporadic params) are valid */
    if ((verify = ksched_ok(thp, policy, param)) != EOK) {
        return verify;
    }

    /* If we are sporadic server, then we need to do some work. We might already
     * have a data structure, in which case we may already be active. If we were
     * active, then we only put the changes in place and expect that others will
     * pick then up over time.
     */
    if (policy == SCHED_SPORADIC) {
        /* Sporatic was specified, and we're not running sporadic .. need
         * ss_info alloced
         *
         * AM Note: Orignally, was a check against !thp->schedinfo.ss_info, and
         * if zero, alloc. not really valid, as it could be non-zero, since the
         * thread is scheduled to run as the inherited policy of the parent,
         * just until we actually force a new policy, thus making rr_ticks
         * increment. Since it's a union, this would result in a false positive
         *
         * So the check should be WAAA state, and that the thread policy isn't
         * SPORADIC initially, since we would have allocated the structure at
         * creation time already
         */
        if (thp->policy != SCHED_SPORADIC) {
            ss_info = object_alloc(thp->process, &ssinfo_souls);
            if (!ss_info) {
                return EAGAIN;
            }

            /* Fake an atomic operation - I need to change the schedinfo union,
             * and if its policy is RR/OTHER, a clock tick on the hart where thp
             * runs could charge rr_ticks over the union and corrupt the ss_info
             * pointer we install just below. Flip to SCHED_FIFO first (a tick
             * then charges nothing — the clock only touches rr_ticks under
             * IS_SCHED_RR), then drain any in-flight tick before installing.
             *
             * WAIT_FOR_CLOCK_DONE is NOT enough here: it only waits the
             * preemptible lockless readers, and clock_handler no longer bumps
             * ticker_using. The clock is non-preemptible interrupt context, so
             * an RCU-sched grace period is what fences it: after the FIFO store
             * it publishes that store globally and waits every hart's in-flight
             * tick out (quiescing on interrupt return), so no tick observes the
             * stale RR policy once we write ss_info. Legal for the same reason
             * as in sched_ss_cleanup() — this path is lock-free.
             */
            thp->policy = SCHED_FIFO;

            rcu_sched_synchronize();

            memset(ss_info, 0, sizeof(*thp->schedinfo.ss_info));
            ss_info->replenishment.thp = thp;
            thp->schedinfo.ss_info = ss_info;
        }

        /* Thread creation and newcomers to the policy need this info snapped */
        if (thp->flags & RTMUX_FLG_THR_WAAA || thp->policy != SCHED_SPORADIC) {
            snap_time(&thp->schedinfo.ss_info->activation_time, 0);
            thp->schedinfo.ss_info->curr_budget =
                timespec2nsec(&param->ss_init_budget);
        }

        /* If there is a change in the attributes, we must make the change and
         * let the ramifications flow through to the other components that deal
         * with the SS alg.
         */
        if (thp->schedinfo.ss_info->low_priority == param->ss_low_priority &&
            thp->schedinfo.ss_info->max_repl == param->ss_max_repl &&
            thp->schedinfo.ss_info->repl_period ==
                timespec2nsec(&param->ss_repl_period) &&
            thp->schedinfo.ss_info->init_budget ==
                timespec2nsec(&param->ss_init_budget)) {
            /* No change, don't do anything */
        } else {
            thp->schedinfo.ss_info->low_priority =
                (uint8_t) param->ss_low_priority;
            thp->schedinfo.ss_info->max_repl = (uint16_t) param->ss_max_repl;
            thp->schedinfo.ss_info->repl_period =
                timespec2nsec(&param->ss_repl_period);
            thp->schedinfo.ss_info->init_budget =
                timespec2nsec(&param->ss_init_budget);

            /* If dropped, then adjust to the low priority, save hi priority */
            if (thp->schedinfo.ss_info->org_priority != 0) {
                thp->schedinfo.ss_info->org_priority = (uint8_t) new_prio;
                new_prio = (unsigned) param->ss_low_priority;
            }
        }
    } else {
        if (thp->policy == SCHED_SPORADIC) {
            thp->policy =
                SCHED_FIFO; /* Temporary since we are nuking SS structures */
            sched_ss_cleanup(thp);
        }
        RR_RESET_TICK(thp);
    }

    /* Change policy and priority after fiddling with attributes */
    thp->policy = (uint8_t) policy;

    /* The base, and the end of a notification loan; any other loan above it
     * holds until it ends. An unchanged priority still sends the caller to the
     * tail of its level, as adjust_priority() did for an equal priority.
     */
    thread_end_notification_loan(thp);
    thread_set_base_prio(thp, new_prio);
    if (!thread_prio_update_dpp(thp, thp->dpp, 0) &&
        thp == get_active_thread(get_cpunum()))
        yield();

    return EOK;
}

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
/** send an ipi to notify that a blocked thread doing msg xfer is force_readied
 */
static void force_ready_msgxfer(thread_t *thp, unsigned err)
{
    /* The claimer passes this back to force_ready() as it stands, error and
     * _FORCE_* bits together: it is not negated.
     */
    SETKSTATUS(thp, err);
    ITF_SET(thp, RTMUX_ITF_MSG_FORCE_RDY);

    /* only in a CPU which is not doing msg xfer could force_ready a thread
     * which is blocked and doing msg xfer search actives[] to see who is doing
     * the msgxfer, and send ipi to preempt that one
     */
    {
        uint64_t online = cpu_online_snapshot();

        for (int _hi = 0; _hi < num_processors; _hi++) {
            unsigned h = (unsigned) _hi;

            if (!cpu_in_mask(online, h))
                continue;

            /* Load the hart's active thread once: it clears to NULL whenever
             * that hart blocks into idle, so a second get_active_thread(h)
             * after the NULL test can return NULL and fault on the field load
             * (seen as a load fault at stval 0x20, the offset of
             * internal_flags).
             */
            thread_t *ath = get_active_thread(h);

            if (!ath)
                continue;
            if (ath->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
                if (((thread_t *) ath->blocked_on) == thp) {
                    SENDIPI(h, IPI_RESCHED);
                    break;
                }
            }
        }
    }
}

/* Whether thp is claimed (MSG_DELIVERY) and its wake was therefore handed to
 * the claimer (MSG_FORCE_RDY). The claim is taken and given up under the
 * channel's lock, and the claimer looks for MSG_FORCE_RDY in the window in
 * which it clears the claim, so the test and the hand-over are one step
 * under that lock: a test made without it can see a claim that is released
 * before the flag is set, and the wake is then nobody's. Without a channel
 * (it is gone, and its queues with it) nothing can be claimed afresh and the
 * plain test stands.
 */
static bool force_ready_deferred(thread_t *thp, channel_t *chp, unsigned err)
{
    bool claimed;

    if (chp)
        kspinlock_lock(&chp->lock);
    claimed = (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) != 0;
    if (claimed) {
        inject_force_ready_claimed(thp);
        force_ready_msgxfer(thp, err);
    }
    if (chp)
        kspinlock_unlock(&chp->lock);
    return claimed;
}
#endif

/* Force a blocked thread out of its wait state.
 *
 * Unwinds whatever the thread is blocked on (message reply/send/receive,
 * mutex/condvar/sem, join, etc.), delivering any required unblock notification,
 * and either readies it (with the given error in its kernel status) or tears
 * down a vthread. Parameters:
 *   thp : thread to force ready
 *   err : error to set, OR'd with _FORCE_* control bits
 * Return value: 1 if the thread was readied/already runnable, 0 if the
 *               unblock was deferred (e.g. awaiting an unblock notification or
 * a cross-CPU message-delivery completion).
 */
int rdecl force_ready(thread_t *thp, unsigned err)
{
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
#ifndef NDEBUG
    if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        /* kprintf("try to force_ready() state %x.\n",thp->state); */
    }
#endif
#endif

    /* If force_ready is passed a non-thread pointer, print the thp address, the
     * wrong type byte and the return address before crashing, so the offending
     * caller can be identified from kernel.sym / addr2line.
     */
    if ((TYPE_MASK(thp->type) != TYPE_THREAD) &&
        (TYPE_MASK(thp->type) != TYPE_VTHREAD)) {
        kprintf(
            "force_ready BAD TYPE: thp=%p type=0x%02x state=%u ra=%lx cpu=%u\n",
            (void *) thp, (unsigned) thp->type, (unsigned) thp->state,
            (unsigned long) __builtin_return_address(0), get_cpunum());
        crash();
    }

    /* The channel whose queue we unlink thp from below, for the per-channel
     * chp->lock. Carried across the STATE_REPLY -> STATE_RECEIVE fall-through:
     * the REPLY case sets it; a direct STATE_RECEIVE entry derives it from
     * thp->blocked_on after the WAIT_PENDING spin. The STATE_SEND case locks
     * cop->channel directly (no fall-through).
     */
    channel_t *fr_chp = NULL;

    /* Set once the STATE_REPLY arm has already unlinked thp from fr_chp's
     * reply_queue under fr_chp->lock, so the shared tail below must not repeat
     * the unlink or the MSG_DELIVERY check.
     */
    bool unlinked = false;

    /* Set when control reaches the STATE_RECEIVE arm from the STATE_REPLY arm;
     * fr_chp (possibly NULL: channel gone) is then authoritative and blocked_on
     * must not be reinterpreted as a channel.
     */
    bool from_reply = false;

    /* Make sure short message flag is cleared */
    if (thp->state != STATE_READY && thp->state != STATE_RUNNING &&
        thp->state != STATE_REPLY) {
        atomic_clr(&thp->flags, RTMUX_FLG_THR_BUFF_MSG);
    }

reread_state:
    switch (thp->state) {
    case STATE_READY:
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
            /* this means that the chanel has been destroyed, put the thread in
             * ready queue
             */
            if (TYPE_MASK(thp->type) == TYPE_VTHREAD) {
                thp->state = STATE_STOPPED;
                snap_time(&thp->timestamp_last_block, 0);
                if (thp->flags & RTMUX_FLG_THR_KILLSELF) {
                    object_free(NULL, &vthread_souls, thp);
                }
            } else {
                ready(thp);
            }
        }
#endif
        /* fall through */
    case STATE_RUNNING:
        thp->restart = NULL; /* Abort any restart on a message pass */
        /* Fall Through */

    case STATE_DEAD:
    case STATE_STOPPED:
        return 1;
    case STATE_WAITPAGE:
    case STATE_STACK:
        return 0;

    case STATE_NET_REPLY:
    case STATE_REPLY: {
        connect_t *cop;
        channel_t *chp;

        /* A sender is linked on reply_queue and in STATE_REPLY before block()
         * has restored blocked_on; WAIT_PENDING covers that window. Reading
         * blocked_on first would see its transient NULL, take the "exchange
         * already completed" return below, and let thread_destroy mark a still
         * linked thread dead.
         */
        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
            __asm__ volatile("" ::: "memory");
        cpu_smp_mb();
        cop = __atomic_load_n(&thp->blocked_on, __ATOMIC_ACQUIRE);

        /* A concurrent reply on another hart can complete the exchange
         * (clearing blocked_on) between our read of thp->state and here; there
         * is then nothing left to unwind (observed as a NULL cop->flags load in
         * the _channel_destroy sender drain at CPUS=8).
         */
        if (!cop)
            return 1;

        if ((TYPE_MASK(thp->type) != TYPE_VTHREAD) &&
            (cop->flags & COF_NETCON)) {
            chp = remote_ipc.chp;
        } else {
            chp = cop->channel;
        }
        fr_chp = chp; /* carried to the LINKPRIL_REM below */
        from_reply = true;

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        if (force_ready_deferred(thp, chp, err))
            return 0;
#endif

        /* If the channel wants an unblock notification and the responsible
         * process _isn't_ dying, we can't unblock this thread just yet.
         */
        if (chp && (chp->flags & RTMUX_FLG_CHF_UNBLOCK) &&
            !(err & _FORCE_NO_UNBLOCK) &&
            ((!chp->process) ||
             !(chp->process->flags &
               (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL))) &&
            (thp->state == STATE_REPLY)) {
            int rcvid;
            if (TYPE_MASK(thp->type) == TYPE_VTHREAD) {
                rcvid = (int) ((((vthread_t *) thp)->un.remote.vtid << 16) |
                               cop->scoid);
            } else if (cop->flags & COF_NETCON) {
                rcvid = (int) ((thp->args.ms.coid << 16) | 0xffff);
            } else {
                rcvid = (int) ((thp->tid << 16) | cop->scoid);
            }
            _TRACE_COMM_EMIT_SNOTIFICATION_UN(
                cop, cop->scoid | RTMUX_SIDE_CHANNEL, thp->priority);

            /* Publish UNBLOCK_REQ before the notification can be received: a
             * multi-threaded server may handle the notification on another
             * worker while the original request is still being dispatched, and
             * must be able to see this flag (msg_info RTMUX_MI_UNBLOCK_REQ)
             * then. Cleared below if the notification cannot be delivered.
             */
            if ((err & _FORCE_SET_ERROR) == 0)
                atomic_or(&thp->flags, RTMUX_FLG_THR_UNBLOCK_REQ);
            if (!(thp->internal_flags & RTMUX_ITF_UNBLOCK_QUEUED)) {
                /* Force SOUL_CRITICAL for the unblock-notification alloc via
                 * the per-CPU soul_alloc_override mechanism — no shared global
                 * mutation, no cross-CPU race. See lock.h.
                 */
                unsigned cpu = get_cpunum();
                pcpu_array[cpu].pc_soul_alloc_override = SOUL_OVERRIDE_CRITICAL;
                if (notification_deliver(
                        chp, thp->priority, _NOTIFICATION_CODE_UNBLOCK,
                        (uintptr_t) rcvid, cop->scoid | RTMUX_SIDE_CHANNEL,
                        0) != EOK) {
                    /* If there was a server fault and the notification could
                     * not be delivered, let the client unblock as if the server
                     * handled the notification.
                     */
                    err |= _FORCE_SET_ERROR;
                } else {
                    ITF_SET(thp, RTMUX_ITF_UNBLOCK_QUEUED);
                }
                pcpu_array[cpu].pc_soul_alloc_override = SOUL_OVERRIDE_NONE;
            }
            if (err & _FORCE_KILL_SELF) {
                atomic_or(&thp->flags,
                          RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME);
            }
            if ((err & _FORCE_SET_ERROR) == 0)
                return 0; /* UNBLOCK_REQ was published above */
            atomic_clr(&thp->flags, RTMUX_FLG_THR_UNBLOCK_REQ);
        }

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        /* Decide between "abort now" and "defer to the claimer" under the
         * channel lock, before any destructive step. A replier / msg_error /
         * msg_write claims thp (MSG_DELIVERY) under this same lock after
         * checking state == STATE_REPLY, so the lock-free test at the top of
         * this arm can lose the race: acting on it here would clear blocked_on
         * and the reply link while the claimer still owns thp, leaving thp
         * STATE_REPLY, still queued, with blocked_on == NULL and a stale
         * MSG_FORCE_RDY, which no path can then wake (the channel destroy drain
         * spins on it forever). Unlinking under the lock, with blocked_on
         * cleared in the same window, makes a later claimer see blocked_on !=
         * cop and back off with ESRCH instead of double-unlinking thp.
         */
        if (fr_chp) {
            /* A sender is on reply_queue only once WAIT_PENDING is clear. */
            while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
                __asm__ volatile("" ::: "memory");
            cpu_smp_mb();
            kspinlock_lock(&fr_chp->lock);
            if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
                force_ready_msgxfer(thp, err);
                kspinlock_unlock(&fr_chp->lock);
                return 0;
            }

            /* Another force_ready or a replier may have completed the exchange
             * since the lock-free reads above; then thp is no longer ours to
             * unlink.
             */
            if ((thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) ||
                thp->blocked_on != cop) {
                kspinlock_unlock(&fr_chp->lock);
                return 1;
            }
            LINKPRIL_REM(thp);
            thp->blocked_on = NULL; /* == cop, checked above under the lock */
            IPC_BLK(thp);
            kspinlock_unlock(&fr_chp->lock);
            unlinked = true;
        }
#endif
        if (thp->args.ms.server) {
            /* The server's next receive clears its own client link and then the
             * client's args.ms.server, as two separate stores. A client abort
             * landing between them sees a server whose client is already NULL
             * (or already a later client); that is the server abandoning this
             * client, not corruption, so do not crash. Clear the server's link
             * only if it still names this thread, with a compare-exchange so a
             * newer client the server has meanwhile taken is not overwritten.
             */
            thread_t *srv = thp->args.ms.server;

            /* The server stops serving thp and its loan ends here; the priority
             * it is owed now, and the chain it may be blocked in, are settled
             * in thread context (thread_prio_defer_settle). This runs from
             * timer and event delivery under caller locks, where taking the PI
             * lock and walking the chain are not allowed.
             */
            inject_in_departure(1);
            if (thread_client_departed(srv, thp))
                thread_prio_defer_settle(srv);
            inject_in_departure(0);
            thp->args.ms.server = NULL;
        }
        if (thp->restart) {
            if ((thp->restart->flags &
                 (RTMUX_FLG_THR_RCVINFO | RTMUX_FLG_THR_SHORT_MSG)) != 0) {
                KCALL_RESTART(thp->restart);
                atomic_clr(&thp->restart->flags,
                           (RTMUX_FLG_THR_RCVINFO | RTMUX_FLG_THR_SHORT_MSG));
                ITF_CLR(thp, RTMUX_ITF_SPECRET_PENDING);
                thp->restart = NULL;
            }
        }

        /* Clear the thread's stale back-reference before the last-link drop can
         * free cop. lookup_rcvid()'s connobj_rcu section protects only the
         * published connection it returned; thp->blocked_on is a separate field
         * that a later reader (ksys_msg_readv &c.) dereferences, so it must not
         * outlive cop. The REPLY fall-through into STATE_RECEIVE uses fr_chp,
         * not blocked_on, so clearing it here is safe.
         */
        if (thp->blocked_on == cop) {
            thp->blocked_on = NULL;
            IPC_BLK(thp);
        }
        if (connect_links_drop(cop)) {
            connect_detach(cop, thp->priority);
        }
    }

        /* Fall Through */

    case STATE_RECEIVE:

        /* Spin-wait for any in-flight channel-waiter transition to complete
         * before observing chp->receive_queue. IPC kcalls set
         * RTMUX_ITF_WAIT_PENDING around their LINKPRIL_BEG / LINKPRIL_REM; the
         * LINKPRIL_REM below runs under channel_slock, released immediately (no
         * nested SCHED).
         */
        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
            __asm__ volatile("" ::: "memory");
        cpu_smp_mb();

        /* Direct STATE_RECEIVE entry: fastmsg.c sets act->blocked_on = chp
         * after the receive block, so blocked_on is the channel here. A
         * STATE_REPLY fall-through already set fr_chp. Lock that channel's
         * per-channel lock around the unlink.
         */
        if (!fr_chp && !from_reply) {
            /* The state read at the top of force_ready is lock-free; thp may
             * have left STATE_RECEIVE since (a sender woke it, it then blocked
             * as a client with blocked_on = a connect_t). Treating that
             * connect_t as a channel locks non-lock memory, so only take
             * blocked_on as a channel after re-checking state and type; the
             * thread has moved on otherwise, with nothing left to unwind.
             */
            void *bo = __atomic_load_n(&thp->blocked_on, __ATOMIC_ACQUIRE);

            if (thp->state != STATE_RECEIVE)
                return 1;
            if (bo && TYPE_MASK(((channel_t *) bo)->type) != TYPE_CHANNEL)
                return 1;
            fr_chp = (channel_t *) bo;
        }

        /* Only unlink when there is a live owning channel to lock. Both reply
         * queue owners are nulled on their own destroy: a thread on a server
         * channel's reply_queue derives fr_chp from cop->channel (nulled by
         * channel_destroy), and a non-vthread netcon waiter, which links onto
         * remote_ipc.chp->reply_queue, derives fr_chp from remote_ipc.chp (the
         * global is nulled when that channel is destroyed, channel.c). So
         * fr_chp == NULL exactly means the queue's channel is gone and freed
         * after its RCU grace period. A sender that linked in the narrow window
         * after the reply drain had already passed is left with thp->prev
         * pointing into that freed channel; a LINKPRIL_REM here would write
         * *(thp->prev) = thp->next through the stale pointer. The freed queue
         * is unreferenced, so abandoning the dangling linkage is safe — thp is
         * being readied and its prev/next are reset the next time it blocks
         * (LINK2_BEG overwrites both). A legitimately queued thread always has
         * a non-NULL owning channel, so this never skips a real unlink.
         * (Residual: such a raced sender is not force-woken by the destroy,
         * only by its own later teardown/timeout — a pre-existing liveness
         * window, not a safety one.)
         */
        if (unlinked) {
            /* The STATE_REPLY arm unlinked thp under fr_chp->lock already. */
        } else if (fr_chp) {
            kspinlock_lock(&fr_chp->lock);
            if (!from_reply &&
                (thp->state != STATE_RECEIVE || thp->blocked_on != fr_chp)) {
                /* Moved on between the lock-free checks and the lock. */
                kspinlock_unlock(&fr_chp->lock);
                return 1;
            }
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            /* A sender claims this receiver (MSG_DELIVERY) under fr_chp->lock;
             * the lock-free test above can lose that race, and unlinking a
             * claimed thread here would be repeated by the claimer.
             */
            if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
                force_ready_msgxfer(thp, err);
                kspinlock_unlock(&fr_chp->lock);
                return 0;
            }
#endif
            LINKPRIL_REM(thp);
            kspinlock_unlock(&fr_chp->lock);
        } else {
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            /* No channel to decide under: the plain test stands. */
            if (!from_reply && force_ready_deferred(thp, NULL, err))
                return 0;
#endif
            /* Skipped the unlink because the owning channel is freed. Null the
             * dangling links (they point into that freed reply_queue) so
             * nothing mistakes thp for still-queued — e.g. exit.c teardown
             * reads prev.pril as a "still linked" flag. next/prev are unions,
             * so this clears the pril view too; the next block re-inits them.
             */
            thp->next.thread = NULL;
            thp->prev.thread = NULL;
        }

        /* Only now is thp ours to abort: every early return above defers to a
         * claimer that still needs the notification flag and restart intact.
         */
        ITF_CLR(thp, RTMUX_ITF_RCVNOTIFICATION);
        thp->restart = NULL; /* Abort any restart on a message pass */
        break;

    case STATE_NET_SEND:
    case STATE_SEND: {
        connect_t *cop;

        /* WAIT_PENDING spin-wait + pril_rem on chp->send_queue under
         * channel_slock. Mirrors the STATE_RECEIVE branch above. The spin comes
         * before blocked_on is read: a sender is linked on send_queue and in
         * STATE_SEND before block() has restored blocked_on, so an earlier read
         * sees a transient NULL, skips the unlink below, and readies a thread
         * that is still queued.
         */
        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
            __asm__ volatile("" ::: "memory");
        cpu_smp_mb();
        cop = thp->blocked_on;

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        if (force_ready_deferred(
                thp,
                cop ? __atomic_load_n(&cop->channel, __ATOMIC_ACQUIRE) : NULL,
                err))
            return 0;
#endif

        /* cop->channel == NULL only after channel_destroy / connect_detach have
         * already force-readied every queued sender (the drain runs before
         * cop->channel is NULLed), so a STATE_SEND thread reaching here with a
         * NULL channel is not on any send_queue and needs no pril_rem — just
         * drop its link and detach. If a future path nulls cop->channel while a
         * sender is still queued, that thread would dangle on the stale list;
         * keep the drain-before-null ordering to preserve this invariant.
         */
        if (!cop || !cop->channel) {
            if (thp->blocked_on == cop) {
                thp->blocked_on = NULL;
                IPC_BLK(thp);
            }
            if (cop && connect_links_drop(cop))
                connect_detach(cop, thp->priority);
            break;
        }
        kspinlock_lock(&cop->channel->lock);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        /* A receiver claims this sender (MSG_DELIVERY) under the same lock; the
         * lock-free test above can lose that race (see the STATE_RECEIVE arm).
         */
        if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
            force_ready_msgxfer(thp, err);
            kspinlock_unlock(&cop->channel->lock);
            return 0;
        }
#endif

        /* A receiver moves a sender from send_queue to reply_queue and flips
         * its state under this lock. A force_ready that read STATE_SEND before
         * that and takes the lock after it must not unlink thp from the wrong
         * queue or drop its link a second time: the exchange moved on and the
         * reply drops the link.
         */
        if ((thp->state != STATE_SEND && thp->state != STATE_NET_SEND) ||
            thp->blocked_on != cop) {
            kspinlock_unlock(&cop->channel->lock);
            return 1;
        }
        pril_rem(&cop->channel->send_queue, thp);

        /* Unlinked and claimed in one lock window: a second force_ready now
         * sees blocked_on != cop and backs off instead of unlinking again. This
         * also clears the back-reference before the last-link drop, which may
         * free cop (see the STATE_REPLY case above).
         */
        thp->blocked_on = NULL; /* == cop, checked above under the lock */
        IPC_BLK(thp);
        kspinlock_unlock(&cop->channel->lock);

        if (connect_links_drop(cop)) {
            connect_detach(cop, thp->priority);
        }
        break;
    }

    case STATE_MUTEX:
    case STATE_CONDVAR:
    case STATE_SEM:
        CRASHCHECK(TYPE_MASK(thp->type) != TYPE_THREAD);

        /* Spin-wait for any in-flight wait-state transition to complete before
         * observing the wait list (see RTMUX_ITF_WAIT_PENDING in macros.h). The
         * trailing fence pairs with the writer's fence-before-flag-clear.
         */
        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
            __asm__ volatile("" ::: "memory");
        cpu_smp_mb();

        /* The waiter can be woken and move on before the chain is locked.
         * A one-shot interrupt belongs only to the wait it observed. */
        inject_force_ready_stall(thp);
        if (!sync_chain_force_ready(thp)) {
            if (!(err & _FORCE_KILL_SELF))
                return 1;

            /* KILLSELF is persistent and runs first at special return.
             * A still-blocked waiter with an empty key belongs to its
             * waker. If it moved to another wait, claim that wait instead. */
            atomic_or(&thp->flags, RTMUX_FLG_THR_KILLSELF);
            unsigned state = __atomic_load_n(&thp->state, __ATOMIC_ACQUIRE);
            if ((state == STATE_MUTEX || state == STATE_SEM ||
                 state == STATE_CONDVAR) &&
                !atomic_load_explicit(&thp->sync_wait_key, memory_order_acquire))
                return 0;
            goto reread_state;
        }
        break;

    case STATE_WAITTHREAD:
    case STATE_JOIN: {
        thread_t *target;
        process_t *prp;

        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
            __asm__ volatile("" ::: "memory");
        cpu_smp_mb();
        target = thp->blocked_on;
        if (target) {
            prp = target->process;
            if (prp)
                kspinlock_lock(&prp->vec_slock);
            if (thp->state == STATE_WAITTHREAD)
                atomic_or(&target->flags,
                          (RTMUX_FLG_THR_KILLSELF | RTMUX_FLG_THR_ONLYME));
            if (target->join == thp)
                target->join = NULL;
            if (prp)
                kspinlock_unlock(&prp->vec_slock);
        }
        break;
    }

    case STATE_SIGSUSPEND:
    case STATE_SIGWAITINFO:
    case STATE_NANOSLEEP:
    case STATE_INTR:
        break;

    default:
        crash();
    }

    if (thp->state != STATE_READY && thp->state != STATE_RUNNING) {
        kcall_err(thp, (int) (err & ~_FORCE_BITS));
        if (TYPE_MASK(thp->type) == TYPE_VTHREAD) {
            CRASHCHECK(thp->state != STATE_SEND && thp->state != STATE_REPLY);
            thp->state = STATE_STOPPED;
            snap_time(&thp->timestamp_last_block, 0);

            _TRACE_TH_EMIT_STATE(thp, STOPPED);
            if (thp->flags & RTMUX_FLG_THR_KILLSELF) {
                CRASHCHECK(vector_lookup(&vthread_vector, thp->un.remote.vtid));
                object_free(NULL, &vthread_souls, thp);
            }
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            if (!(thp->internal_flags & RTMUX_ITF_NET_UNBLOCK) &&
                remote_ipc.chp) {
                /* Notify the remote IPC service to release the client thread.
                 */
                _TRACE_COMM_EMIT_SNOTIFICATION_QUN((-1), thp->priority);
                notification_deliver(remote_ipc.chp, thp->priority,
                                     _NOTIFICATION_CODE_NET_UNBLOCK,
                                     thp->un.remote.vtid, -1, 0);
            }
#endif
        } else {
            ready(thp);
        }
    }

    return 1;
}


/**
 * Block the active thread and scan down the ready queue looking for thread of
 * next highest priority. Assume that active must have been the thread of
 * highest priority or it would not have been running. At least one thread is
 * always ready (the idle thread).
 *
 * Note that this function is called quite a bit, but we don't create a special
 * version for the non-standard scheduler. Instead, the FIND_HIGHEST() macro is
 * used to indirect to the alternate scheduler functions if needed.
 */

/* Note a kill or a cancel posted to a thread this hart has just put into a
 * wait, for kernel_exit to deliver (thread_block_recheck has the protocol).
 *
 * Called under a dispatch lock once thp has its wait state and is no longer
 * this hart's current thread: right after the switch away from it, or from
 * unready() for a caller that was preempted inside its kernel call. thp cannot
 * have been freed yet: it has to run elsewhere and exit first, and its reaper
 * then waits for a quiescent state this hart reports only at its next switch.
 * The pin extends that to the delivery. The fence orders the displacement
 * before the reads, against the requester's fence after it publishes.
 *
 * One thread is noted per pass through the kernel: a kernel call blocks its
 * caller once, and nothing else runs on this hart before kernel_exit. A second
 * note would be dropped, which leaves that thread as it was before this check
 * existed.
 */
static inline void block_recheck_arm(thread_t *thp)
{
    struct pcpu *pc = &pcpu_array[get_cpunum()];

    cpu_smp_mb();
    if (!(thp->flags & RTMUX_FLG_THR_KILLSELF) &&
        !(thp->internal_flags & RTMUX_ITF_CANCEL_POSTED))
        return;
    switch (thp->state) {
    case STATE_RUNNING:
    case STATE_READY:
    case STATE_DEAD:
    case STATE_STOPPED:
    case STATE_STACK:
    case STATE_WAITPAGE:
        /* Not waiting for an event: already woken, exiting (a pin would hold up
         * its own reap), held by job control, or waiting for roottask, which
         * readies it whatever is pending.
         */
        return;
    default:
        break;
    }
    if (!pc->pc_block_recheck && tthread_pin(thp) == EOK)
        pc->pc_block_recheck = thp;
}

/* block takes sched_slock around the dispatch mutation (FIND_HIGHEST,
 * mark_running, set_active_thread). The pre-mutation snap_time/ timeout_start
 * touch per-thread fields only and stay outside the lock. Callers MUST NOT hold
 * sched_slock on entry; cross-callers (unready, block_and_ready_default) drop
 * it before calling.
 */
/* Put back, at the head of its list, a thread select_thread took for this hart
 * that will not run now. Caller holds the local dispatch lock.
 */
static void dispatch_requeue_head(thread_t *thp)
{
    kdispatch_t *dpp = LOCAL_DPP;

    thp->dpp = dpp;
    LINK3_BEG(DISPATCH_LST(dpp, thp->priority), &thp->link3_link, thread_t);
    DISPATCH_SET(dpp, thp);
}

/* An interrupt reached the IST between its interrupt_wait and the switch away
 * from it: consume it and keep running. Called with the local dispatch lock
 * held and the IST still this hart's active thread; drops the lock.
 */
static void block_interrupt_arrived(thread_t *ist)
{
    atomic_clr(&ist->flags, RTMUX_FLG_THR_INTR_PENDING);
    ist->state = STATE_RUNNING;
    CRITICAL_MARK_THREAD(ist);
    _TRACE_TH_EMIT_STATE(ist, RUNNING);
    SCHED_UNLOCK_LOCAL();
    if (ist->timeout_flags)
        timeout_stop(ist);
}

static void block_current(bool interrupt_wait)
{
    thread_t *act = get_active_thread(get_cpunum()), *thp = act;

    chk_dispatch(0);

    snap_time(&(act->timestamp_last_block), 0);

    /* Check for timeout timer */
    if (act->timeout_flags & RTMUX_FLG_TIMEOUT_MASK) {
        timeout_start(act);
    }

    if (interrupt_wait)
        inject_intr_before_switch(act);
    SCHED_LOCK_LOCAL();

    /* The PLIC handler sets PENDING while the IST is still this hart's
     * active thread, even after it published STATE_INTR. Check under the
     * dispatch lock, which masks local IRQs. This first look saves the
     * selection below when the interrupt is already there.
     */
    if (interrupt_wait && (act->flags & RTMUX_FLG_THR_INTR_PENDING)) {
        block_interrupt_arrived(act);
        return;
    }

    /* Get highest priority thread which can run on this processor. */
    FIND_HIGHEST(act);

    /* select_thread drops the dispatch lock, and with it the interrupt mask,
     * while it looks at the other harts for a thread to steal. An interrupt
     * taken in that window still finds the IST as this hart's active thread
     * and only sets PENDING. Look again now that the lock is held through the
     * switch: an IST displaced with PENDING set would sleep in STATE_INTR with
     * its source masked, and nothing would ever wake it.
     */
    if (interrupt_wait) {
        inject_intr_after_select(thp, act);
        if (thp->flags & RTMUX_FLG_THR_INTR_PENDING) {
            /* The IST keeps the hart. A thread taken for it goes back to the
             * head of its list; if it outranks the IST, kernel_exit switches
             * (ksys_interrupt_wait returns through it).
             */
            if (act) {
                dispatch_requeue_head(act);
                intrevent_preemption(act->priority);
#if CONFIG_PROCESSORS_MAX > 1
                kick_idle_hart(act);
#endif
            }
            block_interrupt_arrived(thp);
            return;
        }
    }

    if (!act) {
        /* No runnable thread on this CPU's dispatch queue. Clear actives[cpu]
         * so __kcall detects the mismatch and calls cpu_save_and_exit →
         * kernel_exit → cpu_idle.
         */
        unsigned cpu = get_cpunum();
        sched_account_now(); /* bill before reporting the quiescent state */
        rcu_sched_quiescent();
        set_active_thread(cpu, NULL);
        block_recheck_arm(thp);
        SCHED_UNLOCK_LOCAL();
        return;
    }

    if (IS_SCHED_RR(thp)) {
        if ((thp == act) || (thp->priority < act->priority)) {
            /* preempt */
            RR_ADD_PREEMPT_TICK(thp);
        }
    }

    mark_running(act);
    if (thp != act)
        block_recheck_arm(thp);

    switch (thp->state) {
    case STATE_STOPPED:
    case STATE_WAITPAGE:
    case STATE_STACK:
        break;
    default:
        thp->blocked_on = NULL;
        IPC_BLK(thp);
    }

    /* Mark execution and schedule a replenishment if needed */
    if (thp != act) {
        SS_STOP_RUNNING_LOCKED(thp, (thp->priority < act->priority));
    }

    chk_dispatch(1);
    SCHED_UNLOCK_LOCAL();
}


void rdecl block(void)
{
    block_current(false);
}

void block_interrupt_wait(void)
{
    block_current(true);
}

/* Turnstile-style block: same as block(), but releases the supplied
 * LOCK_CLASS_TURNSTILE lock at the precise moment pc_curthread on this CPU has
 * been updated to the new active (or NULL). After that release, any peer hart
 * that takes the same chain lock observes the waiter on the queue AND
 * pc_curthread != waiter on this CPU — so calling ready(waiter) is safe (no
 * dispatch-double-entry).
 *
 * Rank justification: LOCK_CLASS_TURNSTILE sits BELOW SCHED in rank order, so
 * taking SCHED here while still holding "to_release" is the natural ascending
 * acquisition (TURNSTILE -> SCHED). Out-of-LIFO release of "to_release" between
 * mark_running and SCHED_UNLOCK_LOCAL is permitted by lock_debug.
 *
 * See sync/wait.c file header for the cv_signal-vs-cv_wait
 * dispatch-double-entry race this guards against. Parameters:
 *   to_release : turnstile chain lock to drop once pc_curthread no longer
 *                points at the blocking thread
 */
static void rdecl block_release(int newstate, kspinlock_t *to_release)
{
    thread_t *act = get_active_thread(get_cpunum()), *thp = act;

    chk_dispatch(0);

    snap_time(&(act->timestamp_last_block), 0);

    inject_block_stall(thp);
    SCHED_LOCK_LOCAL();
    thp->state = (uint8_t) newstate;
    _TRACE_TH_EMIT_ANY_STATE(thp, (unsigned) newstate);
    if (act->timeout_flags & (1U << newstate)) {
        /* Arm outside SCHED. Keep the chain lock until displacement and
         * deliver any past-deadline timeout only after releasing it. */
        SCHED_UNLOCK_LOCAL();
        timeout_start_deferred(act);
        SCHED_LOCK_LOCAL();
    }

    FIND_HIGHEST(act);

    if (!act) {
        unsigned cpu = get_cpunum();
        sched_account_now(); /* bill before reporting the quiescent state */
        rcu_sched_quiescent();
        set_active_thread(cpu, NULL);
        block_recheck_arm(thp);

        /* pc_curthread is now NULL on this CPU — safe for peer wakers to ready
         * us via this chain. Release the chain lock.
         */
        kspinlock_unlock(to_release);
        SCHED_UNLOCK_LOCAL();
        timeout_deferred_fire();
        return;
    }

    if (IS_SCHED_RR(thp)) {
        if ((thp == act) || (thp->priority < act->priority)) {
            RR_ADD_PREEMPT_TICK(thp);
        }
    }

    mark_running(act);
    if (thp != act)
        block_recheck_arm(thp);

    /* pc_curthread now points at the new active. Peer wakers observing thp on
     * the chain will see pc_curthread != thp on this CPU. Release the chain
     * lock — out-of-LIFO release relative to SCHED is fine; lock_debug matches
     * by (class, instance).
     */
    kspinlock_unlock(to_release);

    switch (thp->state) {
    case STATE_STOPPED:
    case STATE_WAITPAGE:
    case STATE_STACK:
        break;
    default:
        thp->blocked_on = NULL;
        IPC_BLK(thp);
    }

    if (thp != act) {
        SS_STOP_RUNNING_LOCKED(thp, (thp->priority < act->priority));
    }

    chk_dispatch(1);
    SCHED_UNLOCK_LOCAL();

    /* to_release dropped above; safe to deliver a captured past-deadline
     * timeout fire now (force_ready re-takes the chain lock cleanly).
     */
    timeout_deferred_fire();
}

/* Turnstile-style unready: stamps state and tail-calls block_release, so the
 * supplied chain lock is released only after the dispatcher has moved
 * pc_curthread to a new thread.
 *
 * thp MUST equal get_active_thread(get_cpunum()) — this is the
 * waiter-self-block path. Cross-CPU unready continues to use the regular
 * unready() entry point.
 */
void rdecl unready_release(thread_t *thp, int newstate, kspinlock_t *to_release)
{
    chk_thread(thp);

    /* Same-CPU-only contract. Callers (ksys_sync_mutex_lock slow path,
     * ksys_sync_condvar_wait, ksys_sync_sem_wait) all run with thp == active on
     * the current CPU.
     */
    if (thp != get_active_thread(get_cpunum())) {
        kprintf("FATAL: unready_release non-active thp=%p active=%p\n",
                (void *) thp, (void *) get_active_thread(get_cpunum()));
        crash();
    }

    block_release(newstate, to_release);
}

void __attribute__((noreturn)) rdecl
unready_release_and_exit(thread_t *thp, int newstate, kspinlock_t *to_release)
{
    unready_release(thp, newstate, to_release);
    in_kcall_inc();
    in_kcall_phase(IN_KCALL_PHASE_DISPATCH);
    kernel_exit();
}

/* Make a thread ready to run. If the priority of this thread is higher than
 * active, then make this one active. Ready asumes that if this is true, then it
 * must now be the only thread of that priority which is ready to run, so no
 * searching through the dispatch is necessary.
 */

/* Queue a thread that ready_default is making runnable at the end of its
 * priority list, or, for a round-robin client whose msg_sendvnc just completed,
 * at the head: the server blocks right after the reply, and the client then
 * runs on the CPU and in the cache the server left.
 *
 * A client that blocks in every send is never displaced, so the ticks each head
 * insertion adds were never spent, and its peers at the same priority waited
 * behind it for good. The budget bounds that: once it is used up the client
 * goes to the end and starts over. A FIFO or sporadic thread that becomes
 * runnable goes to the end of its list whatever woke it: it has no round-robin
 * budget to bound the head placement with.
 */
static inline void ready_enqueue_client(kdispatch_t *dpp,
                                        thread_t *thp,
                                        unsigned prio,
                                        bool was_sendvnc)
{
    if (was_sendvnc && IS_SCHED_RR(thp)) {
        RR_ADD_PREEMPT_TICK(thp);
        if (RR_GET_TICKS(thp) < RR_MAXTICKS) {
            LINK3_BEG(DISPATCH_LST(dpp, prio), &thp->link3_link, thread_t);
            return;
        }
    }
    RR_RESET_TICK(thp);
    LINK3_END(DISPATCH_LST(dpp, prio), &thp->link3_link, thread_t);
}

#if CONFIG_PROCESSORS_MAX > 1
/* Make thp READY on another hart's dispatch instance. Called with no dispatch
 * lock held: the local one is dropped first, so the hand-off never holds two
 * and cannot deadlock against a peer doing the reverse. With kick, an idle
 * legal hart is woken when target is busy, since a busy hart only picks from
 * its own queue and may outrank thp.
 */
static void ready_on_hart(thread_t *thp,
                          unsigned target,
                          uint8_t prio,
                          bool was_sendvnc,
                          bool kick)
{
    kdispatch_t *target_dpp = pcpu_array[target].pc_dispatch;

    SCHED_LOCK(target_dpp);
    thp->dpp = target_dpp;
    thp->state = STATE_READY;
    _TRACE_TH_EMIT_STATE(thp, READY);
    ready_enqueue_client(target_dpp, thp, prio, was_sendvnc);
    DISPATCH_SET(target_dpp, thp);
    SENDIPI(target, IPI_RESCHED);

    /* A legal hart that has not joined yet still shows its idle thread but
     * cannot take the IPI, so it counts as busy here.
     */
    if (kick && (!cpu_online(target) ||
                 get_active_thread(target) != pcpu_array[target].pc_idle_thp))
        kick_idle_hart(thp);
    SCHED_UNLOCK(target_dpp);
}

/* The lowest hart thp may run on when its runmask bans this one, else -1. A
 * thread queued on a banned hart is never picked up: idle_pick_thread and
 * select_thread both skip it.
 */
static int banned_here_target(const thread_t *thp)
{
    unsigned legal = (unsigned) (~thp->runmask & LEGAL_CPU_BITMASK);
    unsigned target = 0;

    if (!(thp->runmask & (1u << get_cpunum())) || !legal)
        return -1;
    while (!(legal & (1u << target)))
        target++;
    return (int) target;
}
#endif

/* ready_default takes the per-CPU dispatch lock internally around the
 * dispatch-list mutation; all callers funnel through the same lock.
 *
 * Callers MUST NOT hold sched_slock on entry. Direct callers in this file:
 * force_ready, adjust_priority_default (drops its lock around the internal
 * ready() calls). External callers (sync_wakeup) drop sync_slock first per the
 * SCHED < SYNC rank order.
 */
static void rdecl ready_default(thread_t *thp)
{
    thread_t *act;
    kdispatch_t *dpp;
    uint8_t prio = thp->priority;
    int was_sendvnc = 0;

    chk_dispatch(1);
    chk_thread(thp);
    THR_EV(thp);
    sync_chains_assert_readied(thp);

    if (thp->timeout_flags)
        timeout_stop(thp);

    if (thp->state == STATE_REPLY &&
        _TRACE_GETSYSCALL(thp->syscall) == __KSYS_MSG_SENDVNC) {
        was_sendvnc = 1;
    }

    SCHED_LOCK_LOCAL();

    /* A waiter can lose an unblock race after another CPU has already removed
     * it from its wait queue and queued it READY, but before the losing path
     * notices. In release builds the old code fell through and cleared
     * link3_link before re-enqueueing, leaving the existing ready-list head
     * with !prev; a later steal then crashed in LINK3_REM. Treat duplicate
     * ready as a no-op.
     */
    if (thp->state == STATE_READY) {
        SCHED_UNLOCK_LOCAL();
        return;
    }

    /* Make sure the thread does not have a pending stop */
    if ((thp->flags & RTMUX_FLG_THR_TO_BE_STOPPED) &&
        !(thp->flags & RTMUX_FLG_THR_KILLSELF)) {
        thp->state = STATE_STOPPED;
        snap_time(&thp->timestamp_last_block, 0);
        _TRACE_TH_EMIT_STATE(thp, STOPPED);
        chk_dispatch(1);
        SCHED_UNLOCK_LOCAL();
        return;
    }

    thp->link3_link.next = NULL;
    thp->link3_link.prev = NULL;

#if CONFIG_PROCESSORS_MAX <= 1
    SS_CHECK_EXPIRY(get_active_thread(get_cpunum()));
#endif

    /* If new thread is higher priority we make it active */
    act = get_active_thread(get_cpunum());
    if (!act) {
        /* Set the activation time, clear the consumed, as the path below does
         * for a hart with an active thread.
         */
        SS_MARK_ACTIVATION(thp);

        /* Only idle harts steal from another dispatch queue, so a busy legal
         * hart must be handed thp directly.
         */
#if CONFIG_PROCESSORS_MAX > 1
        {
            int target = banned_here_target(thp);

            if (target >= 0) {
                SCHED_UNLOCK_LOCAL();
                ready_on_hart(thp, (unsigned) target, prio, was_sendvnc, true);
                return;
            }
        }
#endif
        /* No active thread on this CPU; its idle path can pick thp up. */
        thp->state = STATE_READY;
        _TRACE_TH_EMIT_STATE(thp, READY);
        thp->dpp = pcpu_array[get_cpunum()].pc_dispatch;
        dpp = thp->dpp;
        LINK3_END(DISPATCH_LST(dpp, prio), &thp->link3_link, thread_t);
        DISPATCH_SET(dpp, thp);
#if CONFIG_PROCESSORS_MAX > 1
        kick_idle_hart(thp);
#endif
        SCHED_UNLOCK_LOCAL();
        return;
    }

    /* act runs on this CPU, so act->dpp is already this CPU's dispatch
     * instance. thp is enqueued here unless select_cpu picks a different target
     * (the cross-CPU branch below switches thp->dpp accordingly). Snap thp->dpp
     * to the local instance up front; this covers the LAZY_RESCHED,
     * select_cpu==me, fallthrough and UP enqueue branches uniformly.
     */
    dpp = act->dpp;
    thp->dpp = dpp;

#if CONFIG_PROCESSORS_MAX > 1
    {
        int cpu;

        /* Set the activation time, clear the consumed */
        SS_MARK_ACTIVATION(thp);

        /* Fires whenever the readying CPU is banned, regardless of what
         * select_cpu below would pick.
         */
        if ((cpu = banned_here_target(thp)) >= 0) {
            SCHED_UNLOCK_LOCAL();
            ready_on_hart(thp, (unsigned) cpu, prio, was_sendvnc, true);
            return;
        }

        if (STATE_LAZY_RESCHED(thp) && (prio <= act->priority) &&
            (thp->runmask & (1u << get_cpunum())) == 0) {
            /* To cut down on inter-core thrashing we try to keep client and
             * server threads on the same core.
             *
             * Typically the server will block right after making the client
             * ready and then the client will run.
             */
            thp->state = STATE_READY;
            _TRACE_TH_EMIT_STATE(thp, READY);
            ready_enqueue_client(dpp, thp, prio, was_sendvnc);
            DISPATCH_SET(dpp, thp);
#if CONFIG_PROCESSORS_MAX > 1
            /* The "server blocks right after" assumption fails when the server
             * keeps running (its own helper threads, IRQ/timer work): the
             * just-readied client would then starve on this busy hart's queue
             * while sibling harts sleep in wfi — a lost wakeup. Kick one idle
             * hart so it steals the client. If none is idle, keep the client
             * local as intended (all harts busy ⇒ no migration win anyway).
             */
            kick_idle_hart(thp);
#endif
            SCHED_UNLOCK_LOCAL();
            return;
        }
        if ((cpu = select_cpu(thp)) >= 0) {
            /* We have found a CPU for which this thread is the highest priority
             */

            /* Do we need to assign another cpu this thread. */
            if (cpu != get_cpunum()) {
                /* Cross-CPU enqueue. Route thp onto the TARGET hart's per-CPU
                 * dispatch instance and lock the TARGET's dispatch lock for the
                 * mutation. Hand-off model: drop the local lock first, then
                 * take target's — the mutation only touches target's queue, so
                 * there is no need to hold both, which avoids ABBA between any
                 * pair of harts.
                 */
                SCHED_UNLOCK_LOCAL();
                ready_on_hart(thp, (unsigned) cpu, prio, was_sendvnc, false);
                return;
            }

            /* The target CPU is the current CPU (cpu == get_cpunum()). Since
             * the inline REPLACE-current-active path below is disabled, we must
             * signal preemption via async_flags so that kernel_exit dispatches
             * the higher-priority thp at trap return.
             */
            if (prio > act->priority) {
                atomic_fetch_or_explicit(&act->async_flags,
                                         RTMUX_ATF_SMP_RESCHED,
                                         memory_order_release);
            }

            /* The REPLACE-current-active path is disabled (the "0 &&" below).
             * It displaced act mid-kcall — setting act->state=READY and
             * LINK3_BEGing act onto the dispatch queue — which exposed act to
             * idle_pick steal on another CPU before this CPU finished act's
             * kcall body and dispatched via kernel_exit, so two CPUs could run
             * the same thread_t. It was also unsafe when act was mid sync-wait
             * transition (RTMUX_ITF_WAIT_PENDING): act's next/prev slots are
             * then in use as the wait-list PRIL entry, and LINK3_BEG would
             * corrupt the wait list.
             *
             * Instead, fall through: thp is enqueued normally and kernel_exit
             * at the end of act's kcall calls FIND_HIGHEST to dispatch thp.
             */
            if (0 && !(act->internal_flags & RTMUX_ITF_WAIT_PENDING)) {
                /* The thread can replace the thread on this processor core. */
                mark_running(thp);

                thp = act;

                /* If the preempted thread is THIS CPU's idle/boot thread, do
                 * NOT enqueue it on the dispatch queue. Its saved register
                 * context is the one from kernel/init/objects.c (SETKIP(thp,
                 * &idle)) and was never updated as the boot CPU ran on into
                 * cpu_idle()'s C body — re-dispatching via __kernel_restore
                 * would sret back to idle()'s entry, reentering
                 * start_kernel_threads → ksys_start_roottask in an infinite
                 * recursion (each round leaks a fresh process_t/thread_t). The
                 * idle thread is "owned" by the CPU's idle loop, not the
                 * scheduler: when no other thread is runnable, cpu_idle's wfi
                 * takes over directly.
                 */
                if (act == pcpu_array[get_cpunum()].pc_idle_thp) {
                    /* No chk_dispatch() here: the RTMux idle is "owned" by
                     * cpu_idle's wfi loop, not the scheduler. Once preempted it
                     * goes to neither the ready list nor the active slot, which
                     * violates the chk_dispatch invariant "no prio-0 ready ⇒
                     * active is prio-0".
                     */
                    SCHED_UNLOCK_LOCAL();
                    return;
                }
                RR_ADD_PREEMPT_TICK(act);
                act->state = STATE_READY;
                LINK3_BEG(DISPATCH_LST(dpp, act->priority), &act->link3_link,
                          thread_t);

                SS_STOP_RUNNING(act, 1);
                goto enqueue_done;
            }
        }
        thp->state = STATE_READY;

        /* If new thread is lower or equal prio, it goes at end of it's ready
         * queue, unless it was blocked due to a msg_sendvnc call (see the above
         * description of this issue.
         */
        ready_enqueue_client(dpp, thp, prio, was_sendvnc);
    enqueue_done:;
    }
#else

    /* Set the activation time, clear the consumed on ready or running */
    SS_MARK_ACTIVATION(thp);

    /* UP variant of the SMP guard above: the REPLACE path is disabled (the "0
     * &&" below). See the SMP variant for the rationale.
     */
    if (0 && prio > act->priority &&
        !(act->internal_flags & RTMUX_ITF_WAIT_PENDING)) {
        mark_running(thp);
        thp = act;

        /* If the preempted thread is THIS CPU's idle/boot thread, do NOT
         * enqueue it on the dispatch queue. Its saved register context is the
         * one from kernel/init/objects.c (SETKIP(thp, &idle)) and was never
         * updated as the boot CPU ran on into cpu_idle()'s C body —
         * re-dispatching via __kernel_restore would sret back to idle()'s
         * entry, reentering start_kernel_threads → ksys_start_roottask in an
         * infinite recursion (each round leaks a fresh process_t/thread_t). The
         * idle thread is "owned" by the CPU's idle loop, not the scheduler:
         * when no other thread is runnable, cpu_idle's wfi takes over directly.
         */
        if (act == pcpu_array[get_cpunum()].pc_idle_thp) {
            chk_dispatch(1);
            SCHED_UNLOCK_LOCAL();
            return;
        }

        /* If new thread is higher prio, the old active is put at head of it's
         * ready queue (think fifo scheduling).
         */
        RR_ADD_PREEMPT_TICK(act);
        act->state = STATE_READY;
        LINK3_BEG(DISPATCH_LST(dpp, act->priority), &act->link3_link, thread_t);

        SS_STOP_RUNNING(act, 1);
    } else {
        thp->state = STATE_READY;

        /* If new thread is lower or equal prio, it goes at end of it's ready
         * queue, unless it was blocked due to a msg_sendvnc call (see the above
         * description of this issue.
         */
        ready_enqueue_client(dpp, thp, prio, was_sendvnc);
    }
#endif
    _TRACE_TH_EMIT_STATE(thp, READY);
    DISPATCH_SET(dpp, thp);
    chk_dispatch(1);
    SCHED_UNLOCK_LOCAL();
}


/* Block the active thread and ready the specified thread. If the new thread is
 * of higher priority than active, then it can be made active immediately. If
 * not, we must scan the dispatch looking for the highest priority ready thread.
 */

/* block_and_ready_default takes the dispatch lock around the dispatch- list
 * mutation (LINK3_*, FIND_HIGHEST, mark_running). If the thread has a pending
 * stop, the body degrades to a plain block() — and drops the lock first so the
 * inner block() can acquire it itself.
 */
static void rdecl block_and_ready_default(thread_t *thp)
{
    thread_t *act = get_active_thread(get_cpunum());
    thread_t *old = act; /* a sender leaves in STATE_REPLY through here */
    kdispatch_t *dpp;
    uint8_t prio;

    chk_dispatch(0);
    chk_thread(thp);
    inject_block_stall(act);

    /* Check for timeout timers */
    if (act->timeout_flags & RTMUX_FLG_TIMEOUT_MASK) {
        timeout_start(act);
    }

    if (thp->timeout_flags)
        timeout_stop(thp);

    thp->restart = NULL;

    /* If thp has a pending stop then degrade block_and_ready() to just block()
     */
    if ((thp->flags & RTMUX_FLG_THR_TO_BE_STOPPED) &&
        !(thp->flags & RTMUX_FLG_THR_KILLSELF)) {
        thp->state = STATE_STOPPED;
        _TRACE_TH_EMIT_STATE(thp, STOPPED);
        /* block() takes sched_slock — call it before our acquire. */
        block();
        chk_dispatch(1);
        return;
    }

    SCHED_LOCK_LOCAL();

    thp->link3_link.next = NULL;
    thp->link3_link.prev = NULL;

    SNAP_TIME_INLINE(act->timestamp_last_block, 0);

    /* Queue on this hart's instance, the one SCHED_LOCK_LOCAL() holds, as
     * ready_default does. thp->dpp may be another hart's (a thread created for
     * another process gets that process's default); linking into it under our
     * lock tears its LINK3 list.
     */
    dpp = LOCAL_DPP;
    thp->dpp = dpp;
    prio = thp->priority;

    /* We are blocking the active thread (externally) so stop the running Mark
     * the activation time for this new thread
     */
    SS_STOP_RUNNING_LOCKED(
        act, ((act->state == STATE_READY) || (act->state == STATE_RUNNING)));
    SS_MARK_ACTIVATION(thp);

#if CONFIG_PROCESSORS_MAX > 1
    /* Should the new thread run on this cpu. */
    if ((thp->runmask & (1u << get_cpunum())) == 0) {
        if (prio > act->priority || prio > DISPATCH_HIGHEST_PRI(dpp)) {
            mark_running(thp);
            block_recheck_arm(old);
            chk_dispatch(1);
            SCHED_UNLOCK_LOCAL();
            return;
        }
    } else {
        int cpu;

        if ((cpu = select_cpu(thp)) >= 0) {
            /* The thread should run on another processor. */
            if (cpu == get_cpunum())
                crash();
            SENDIPI((unsigned) cpu, IPI_RESCHED);
        }
    }
#else
    /* Will thp be the highest priority thread ready to run? */
    if (prio > act->priority || prio > DISPATCH_HIGHEST_PRI(dpp)) {
        mark_running(thp);
        block_recheck_arm(old);
        chk_dispatch(1);
        SCHED_UNLOCK_LOCAL();
        return;
    }
#endif

    /* Add thp to ready queue. */
    if (thp->policy == SCHED_FIFO) {
        /* FIFO threads must be queued at the end of the list to ensure we don't
         * cause this thread to run before any other FIFO threads that are
         * runnable at this priority
         */
        LINK3_END(DISPATCH_LST(dpp, prio), &thp->link3_link, thread_t);
    } else {
        LINK3_BEG(DISPATCH_LST(dpp, prio), &thp->link3_link, thread_t);
    }
    DISPATCH_SET(dpp, thp);
    thp->state = STATE_READY;
    _TRACE_TH_EMIT_STATE(thp, READY);
    FIND_HIGHEST(act);
    if (!act) {
        sched_account_now();
        rcu_sched_quiescent();
        set_active_thread(get_cpunum(), NULL);
        block_recheck_arm(old);
        chk_dispatch(1);
        SCHED_UNLOCK_LOCAL();
        return;
    }

    mark_running(act);
    if (act != old)
        block_recheck_arm(old);

    chk_dispatch(1);
    SCHED_UNLOCK_LOCAL();
}


/**
 * Remove a thread from the ready list. If the thread being make unready is the
 * active thread we call block() otherwise the thread being made unready is not
 * active. As such, active must be higher priority than the thread being made
 * unready. Therefore we do not need to change active.
 */
/* unready takes the dispatch lock around the dispatch-list removal (LINK3_REM /
 * DISPATCH_CLR) and the per-thread state mutation. When thp is the active
 * thread, the body just sets state and tail-calls block() — dropping the lock
 * first so block() can acquire it itself (non-reentrant kspinlock).
 */
void rdecl unready(thread_t *thp, int newstate)
{
    kdispatch_t *thp_dpp;

    chk_thread(thp);

    /* Lock the dispatch instance thp is associated with. For the
     * active-on-this-CPU case that is LOCAL_DPP; for the non-active case it is
     * thp->dpp (may be another hart's instance). Read thp->dpp BEFORE acquiring
     * its lock — a subsequent re-check would be needed if migrations under
     * other locks could move thp out from under us, but state transitions that
     * change thp->dpp (ready_default / mark_running / select_thread steal) all
     * hold the same lock we're about to acquire, so the value is stable from
     * this acquire onward.
     */
    if (thp == get_active_thread(get_cpunum())) {
        inject_block_stall(thp);
        SCHED_LOCK_LOCAL();

        /* Relaxed invariant. In the BKL era, thp's state was always RUNNING
         * when it called unready (BKL serialized state transitions). Under
         * fine-grained locking thp->state may already carry a partial
         * transition value (e.g. STATE_RECEIVE left over from a kcall that
         * unblocked into RUNNING without updating the state field). Stamp the
         * new state and proceed regardless.
         */
        thp->state = (uint8_t) newstate;
        _TRACE_TH_EMIT_ANY_STATE(thp, (unsigned) newstate);
        /* block() takes LOCAL_DPP->lock — drop ours around the tail-call. */
        SCHED_UNLOCK_LOCAL();
        block();
        return;
    }

    thp_dpp = thp->dpp;
    SCHED_LOCK(thp_dpp);

    /* STATE_RUNNING in unready with thp != actives[cpu] is the
     * "displaced-old-active" case (a higher-prio waiter took the CPU via
     * mark_running while we were still in the kcall body). thp->runcpu is not
     * reliably current (only mark_running + a couple of fast-paths set it), so
     * we don't try to distinguish a same-CPU residual from a cross-CPU race.
     * Stamp the new state; force_ready issues an IPI for any genuine cross-CPU
     * correction.
     */
    if (thp->state == STATE_RUNNING) {
        thp->state = (uint8_t) newstate;
        _TRACE_TH_EMIT_ANY_STATE(thp, (unsigned) newstate);
        block_recheck_arm(thp); /* preempted inside its call, blocks now */
        SCHED_UNLOCK(thp_dpp);
        return;
    }

    /* Only remove from dispatch list if READY */
    if (thp->state == STATE_READY) {
        LINK3_REM(DISPATCH_LST(thp_dpp, thp->priority), &thp->link3_link,
                  thread_t);
        if (!DISPATCH_THP(thp_dpp, thp->priority)) {
            DISPATCH_CLR(thp_dpp, thp);
        }
    }

    SNAP_TIME_INLINE(thp->timestamp_last_block, 0);

    thp->state = (uint8_t) newstate;
    _TRACE_TH_EMIT_ANY_STATE(thp, (unsigned) newstate);

    /* Check for timeout timer */
    if (thp->timeout_flags & RTMUX_FLG_TIMEOUT_MASK) {
        timeout_start(thp);
    }

    block_recheck_arm(thp);
    chk_dispatch(1);
    SCHED_UNLOCK(thp_dpp);
}

/**
 * The yield call implements the __sched_yield() kernel call. The global
 * rescheduling is handled through the resched() call below. Since this is one
 * of the top benchmarked microkernel calls, we special-case a UP and SMP. AP
 * has its own version.
 */


/* A kcall that asked for its own displacement to wait (see
 * RTMUX_ITF_DEFER_DISPLACE): post it for resched() at kernel_exit, which runs
 * after the trap handler has stored the kcall's status.
 *
 * Returns whether it was deferred.
 */
static bool sched_defer_displacement(thread_t *act)
{
    if (!(act->internal_flags & RTMUX_ITF_DEFER_DISPLACE))
        return false;
    ITF_SET(act, RTMUX_ITF_YIELD_TAIL);
    atomic_fetch_or_explicit(&act->async_flags, RTMUX_ATF_SMP_RESCHED,
                             memory_order_release);
    return true;
}

static void rdecl yield_default(void)
{
    thread_t *act = get_active_thread(get_cpunum());
    thread_t *thp;
    uint8_t prio = act->priority;
    kdispatch_t *dpp = act->dpp;
#if CONFIG_PROCESSORS_MAX > 1
    uint32_t runmask = 1u << get_cpunum();
#endif

    /* Mirror the resched_default WAIT_PENDING guard. yield() can be invoked
     * from inside a sync kcall body via adjust_priority_default at line 2257
     * (when thp == act and priorities match) — this is the priority-inheritance
     * "I just want to give other ready threads at my level a chance" yield. If
     * act is mid-cv_wait / mutex_lock (WAIT_PENDING set), swapping it out
     * demotes a kernel-stack-resident thread to STATE_READY
     * + LINK3 dispatch, exposing it to cross-CPU idle_pick steal
     * (dispatch-double-entry). See the resched_default banner for the full race
     * description.
     */
    if (act->internal_flags & RTMUX_ITF_WAIT_PENDING) {
        return;
    }

    if (act->policy == SCHED_FIFO) {
        SCHED_LOCK_LOCAL();
#if CONFIG_PROCESSORS_MAX > 1
        for (thp = DISPATCH_THP(dpp, prio); thp;
             thp = link3_to_thread(thp->link3_link.next)) {
            if ((thp->runmask & runmask) == 0)
                break;
        }
#else
        thp = DISPATCH_THP(dpp, prio);
#endif
        if (!thp || sched_defer_displacement(act)) {
            SCHED_UNLOCK_LOCAL();
            return;
        }
        LINK3_REM(DISPATCH_LST(dpp, prio), &thp->link3_link, thread_t);
        LINK3_END(DISPATCH_LST(dpp, prio), &act->link3_link, thread_t);
        RR_RESET_TICK(act);
        sched_account_expose(); /* single-writer: bill before idle_pick can
                                   steal act
                                   */
        act->state = STATE_READY;
        _TRACE_TH_EMIT_STATE(act, READY);
        SCHED_UNLOCK_LOCAL();
        mark_running(thp);
        SS_STOP_RUNNING(act, 0);
        SS_MARK_ACTIVATION(act);
        chk_dispatch(1);
        return;
    }

    if (IS_SCHED_RR(act))
        act->schedinfo.rr_ticks = RR_MAXTICKS;
    atomic_fetch_or_explicit(&act->async_flags, RTMUX_ATF_TIMESLICE,
                             memory_order_release);
}

/**
 * This function is called on a clock tick (or from another CPU) when we need to
 * do a global scheduling operation.
 */
static void rdecl resched_default(void)
{
    thread_t *act = get_active_thread(get_cpunum());
    thread_t *thp;
    kdispatch_t *dpp = act->dpp;
    uint8_t prio = act->priority;
    bool yield_tail;

    /* A deferred displacement (see RTMUX_ITF_DEFER_DISPLACE): act asked to go
     * behind the threads at its new level, so those are eligible too, and it
     * queues at the tail like a thread whose slice ended.
     */
    yield_tail = (act->internal_flags & RTMUX_ITF_YIELD_TAIL) != 0;
    if (yield_tail)
        ITF_CLR(act, RTMUX_ITF_YIELD_TAIL);

    /* DO NOT preempt a thread that is mid-sync- transition. The WAIT_PENDING
     * flag is set inside cv_wait / mutex_lock / sem_wait between
     * "sync_enqueue(chain, act)" and the final "unready + blocked_on +
     * cpu_smp_mb + flag clear" sequence. Demoting act during that window
     *   - sets act->state = STATE_READY,
     *   - LINK3s act onto cpu1's dispatch queue,
     * exposing act to idle_pick_thread on another CPU — which then
     * mark_runnings act on cpu2 while cpu1's cv_wait body is still mid-frame on
     * cpu1's kernel stack. Two CPUs end up "running" the same thread
     * (dispatch-double-entry), and the cv_wait body on cpu2 sees act already on
     * its sync chain → CV_WAIT REENTRY.
     *
     * Under the BKL era this was masked because BKL acquire disabled SIE: timer
     * ISR couldn't fire and call resched_default. Under the Sync*-only no-BKL
     * carve-out, SIE stays on through cv_wait body and the race opens.
     * WAIT_PENDING is the precise "mid-critical-transition" indicator already
     * used by force_ready, sync_wakeup, and adjust_priority; respect it here
     * too.
     */
    if (act->internal_flags & RTMUX_ITF_WAIT_PENDING) {
        return;
    }

    /* Run down this priority queue looking for a thread which will run on this
     * processor. If we are coming in from an ISR where the expiry of the
     * execution budget of a SS thread was detected, then look at rescheduling
     * and potentially drop the priority of the active thread. If this is done
     * then don't bother with the rest of the yield operation since actives has
     * already been re-evaluated
     *
     * SS_CHECK_EXPIRY dereferences act's schedinfo.ss_info, and this runs
     * lockless with interrupts enabled — so it can pass an RCU-sched quiescent
     * state mid-access (SS_CHECK_EXPIRY may context-switch via sched_ss_update
     * -> adjust_priority), and the grace period in sched_ss_cleanup() would not
     * cover it. Bracket it with the ticker_using reader count so
     * WAIT_FOR_CLOCK_DONE() in sched_ss_cleanup() waits us out instead; the
     * atomic add/sub tolerates a timer tick nesting this window.
     */
    if (__atomic_load_n(&act->policy, __ATOMIC_RELAXED) == SCHED_SPORADIC) {
        __atomic_fetch_add(&act->ticker_using, 1u, __ATOMIC_RELAXED);
        __cpu_membarrier();
        /* Recheck the policy after publishing the reader. A concurrent
         * switch away may already have retired the old budget. */
        SS_CHECK_EXPIRY(act);
        __cpu_membarrier();
        __atomic_fetch_sub(&act->ticker_using, 1u, __ATOMIC_RELAXED);
    }

    /* If there is a replenishment pending, then do the adjustment If actives
     * has changed above, early out.
     */
    if ((ss_replenish_list && sched_ss_adjust() != 0) ||
        (act != get_active_thread(get_cpunum()))) {
        return;
    }

    CRASHCHECK(get_active_thread(get_cpunum()) != act);

    /* check if this process exceeded its max cpu usage. Stop the whole process
     * as a fatal SIGXCPU, the way an unhandled fault does (deliver_fault marks
     * every thread KILLSELF and force-readies the blocked ones): killing only
     * this thread would leave the process's other threads (the libc system
     * thread at least) alive and unreaped. The tick (clock_handler) raises
     * TIMESLICE for every over-limit thread, so sibling threads on several
     * harts can arrive here together; the compare-exchange that disarms the
     * limit (max_cpu_time = 0, "none") lets exactly one of them deliver.
     */
    {
        uint64_t cpu_max =
            __atomic_load_n(&act->process->max_cpu_time, __ATOMIC_RELAXED);

        if (cpu_max != 0 &&
            __atomic_load_n(&act->process->running_time, __ATOMIC_RELAXED) >
                cpu_max &&
            __atomic_compare_exchange_n(&act->process->max_cpu_time, &cpu_max,
                                        0, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
            siginfo_t info;

            memset(&info, 0, sizeof info);
            info.si_signo = SIGXCPU;
            info.si_code = CLD_KILLED;
            info.si_status = SIGXCPU;
            deliver_fault(act, &info);

            /* The kernel-exit loop only re-reads the thread's flags when the
             * active thread changed, and the limit is now disarmed, so no later
             * tick would flag a lone spinner. Re-post the flag so the next
             * interrupt re-enters the kernel and takes the KILLSELF special
             * return.
             */
            atomic_fetch_or_explicit(&act->async_flags, RTMUX_ATF_TIMESLICE,
                                     memory_order_release);
            return;
        }
    }

    /* Find highest priority thread which can run on this processor. Unlink from
     * ready queue if its priority is >= than actives. Note that since
     * SCHED_FIFO threads cannot, by definition be punted from the processor
     * except by a higher priority thread, we restrict the priority we can
     * select from to those higher than act's priority in that case.
     */
    prio = (uint8_t) ((act->runmask & (1u << get_cpunum())) ? 0
                      : (act->policy == SCHED_FIFO && !yield_tail)
                          ? min(act->priority + 1, NUM_PRI - 1)
                          : act->priority);

    /* Same dispatch-list-mutation gap as yield_default. Hold sched_slock across
     * select_thread (which does LINK3_REM internally) and the LINK3_BEG/END for
     * act below. Drop before mark_running (which takes its own sched_slock).
     */
    SCHED_LOCK_LOCAL();
    thp = select_thread((int) get_cpunum(), prio);

    /* If thp is non-null, then select_thread has found someone equal or better
     * to run, although the thread's priority may be lower than what we're
     * currently running.
     */
    if (thp) {
        RR_ADD_PREEMPT_TICK(act);

        /* Add active to ready queue. If tick count >= RR_MAXTICKS, the
         * assumption is we've consummed our sched quantum. Otherwise, we've
         * been preempted and go back to the head of the queue.
         */
        if (yield_tail ||
            (IS_SCHED_RR(act) && RR_GET_TICKS(act) >= RR_MAXTICKS)) {
            RR_RESET_TICK(act);
            LINK3_END(DISPATCH_LST(dpp, act->priority), &act->link3_link,
                      thread_t);
        } else {
            /* otherwise keep active at the head of the queue */
            LINK3_BEG(DISPATCH_LST(dpp, act->priority), &act->link3_link,
                      thread_t);
        }
        DISPATCH_SET(dpp, act);

        sched_account_expose(); /* single-writer: bill before idle_pick can
                                   steal act
                                   */
        act->state = STATE_READY;
        _TRACE_TH_EMIT_STATE(act, READY);
        SCHED_UNLOCK_LOCAL();
        mark_running(thp);
    } else {
        SCHED_UNLOCK_LOCAL();
#if CONFIG_PROCESSORS_MAX > 1
        _TRACE_TH_EMIT_STATE(act, RUNNING);
        VALIDATE_NEEDTORUN(0)
#endif
    }
    chk_dispatch(1);
}

/**
 * Adjust the priority of a thread to the new value of prio This may involve
 * changing the active thread.
 *
 * The priority_inherit parameter is used to indicate whether this adjustment is
 * the result of a priority inheritance protocol. If so, then when dropping
 * priority we consider it a preemptation and place the thread at the begining
 * of the ready queue. Otherwise it goes on the end of the queue and gets it's
 * round robin information reset.
 */
/* adjust_priority_default takes sched_slock around its dispatch-list mutations.
 * It drops the lock around two sub-calls that may not be held under
 * sched_slock:
 *   - yield() — context-switches; spinlock cannot span that.
 *   - ready() — itself takes sched_slock internally; would
 *     recurse-deadlock on a non-reentrant kspinlock.
 * Callers MUST NOT hold sched_slock on entry.
 */
static void rdecl adjust_priority_default(thread_t *thp,
                                          unsigned prio,
                                          kdispatch_t *newdpp,
                                          int priority_inherit)
{
    thread_t *act = get_active_thread(get_cpunum());
    kdispatch_t *dpp;
    kdispatch_t *outer_dpp;

    chk_dispatch(1);
    chk_thread(thp);

    if (thp->priority == prio) {
        if (thp == act) {
            yield();
        }
        return;
    }

    /* Lock the dispatch instance thp is on (may be a remote hart's
     * percpu_dispatch[]). The body may LINK3_REM/BEG on thp's queue or call
     * ready(thp), so the outer lock must cover thp's dispatch. ready() drops
     * this lock internally for its own SCHED acquire path. We use thp_dpp
     * consistently for lock/unlock; the value snapshotted here stays the lock
     * we release at the function tail even if ready() relocated thp.
     */
    outer_dpp = thp->dpp;
    SCHED_LOCK(outer_dpp);

    dpp = thp->dpp;
    switch (thp->state) {
    case STATE_READY:
#ifndef NDEBUG
        /* CRASHCHECK(thp == act), naming the thread. crash_halt writes to the
         * crash buffer, so it never waits on the console lock under SCHED_LOCK.
         */
        if (thp == act)
            crash_halt(
                "sched: priority %u -> %u of the active thread %d.%d, "
                "which is READY (runcpu %u, internal flags 0x%x, "
                "flags 0x%x)",
                (unsigned) thp->priority, (unsigned) prio,
                thp->process ? (int) thp->process->pid : -1, (int) thp->tid,
                (unsigned) thp->runcpu, (unsigned) thp->internal_flags,
                (unsigned) thp->flags);
#endif
        chk_thread(thp);

        /* Unlink from the ready queue */
        LINK3_REM(DISPATCH_LST(dpp, thp->priority), &thp->link3_link, thread_t);
        if (!DISPATCH_THP(dpp, thp->priority)) {
            DISPATCH_CLR(dpp, thp);
        }

        thp->priority = (uint8_t) prio;
#if CONFIG_PROCESSORS_MAX > 1

        /* pretty hokey, but ready_default() returns at once for a thread that
         * is already READY, and this one was just taken off its queue. Done in
         * every build: without it the thread would be dropped here.
         */
        thp->state = STATE_DEAD;

        /* ready() takes its own per-CPU dispatch lock — drop ours around it.
         * After ready() returns, thp may have been routed to a different hart's
         * queue; outer_dpp is the dispatch we entered with, and re-acquiring it
         * is OK because the function tail only chk_dispatch's (walks all dpps)
         * and exits.
         */
        SCHED_UNLOCK(outer_dpp);
        ready(thp);
        SCHED_LOCK(outer_dpp);
#else
        if (thp->priority > act->priority) {
            sched_account_expose(); /* single-writer: bill before idle_pick can
                                       steal act
                                       */
            act->state = STATE_READY;
            _TRACE_TH_EMIT_STATE(act, READY);
            RR_ADD_PREEMPT_TICK(act);
            mark_running(thp);
            thp = act;

            /* This guy should go to the head of the priority list, not the tail
             */
            LINK3_BEG(DISPATCH_LST(dpp, thp->priority), &thp->link3_link,
                      thread_t);

            SS_STOP_RUNNING(act, 1);
        } else {
            /* Put lower of (act, thp) to the end of the ready queue for its
             * priority
             */
            RR_RESET_TICK(thp);
            LINK3_END(DISPATCH_LST(dpp, thp->priority), &thp->link3_link,
                      thread_t);
        }

        DISPATCH_SET(dpp, thp);
#endif
        chk_dispatch(1);
        break;

    case STATE_RUNNING:
        chk_thread(thp);
#if CONFIG_PROCESSORS_MAX > 1
        /* Thread is running on another processor so kick it to adjust. */
        if (act != thp) {
            thp->priority = (uint8_t) prio;
            SENDIPI(thp->runcpu, IPI_RESCHED);
            break;
        }
#else
        CRASHCHECK(thp != act);
#endif

        /* A priority loan changing under the running thread: never switch away
         * from it here. The caller is inside one of act's own kernel calls (a
         * condition wait giving up a PI mutex, an unlock, an IPC reply), which
         * has to finish, and may block act with unready_release(), which
         * requires act to be the active thread. Displacing act would also put a
         * thread still running on this stack on a ready queue, where another
         * hart can pick it up. Lower the priority and post a reschedule:
         * kernel_exit, or the return from the interrupt, runs resched(), which
         * moves to a thread that is due and puts act at the head of its new
         * queue, as a preempted thread.
         */
        if (priority_inherit) {
            if (prio < act->priority) {
                atomic_fetch_or_explicit(&act->async_flags,
                                         RTMUX_ATF_SMP_RESCHED,
                                         memory_order_release);
            }
            act->priority = (uint8_t) prio;
            _TRACE_TH_EMIT_ANY_STATE(act, act->state);
            chk_dispatch(1);
            break;
        }
#if CONFIG_PROCESSORS_MAX > 1
        /* Thread is running on this processor. */
        if (prio < act->priority) {
            /* If act is mid-sync-transition (WAIT_PENDING set), DO NOT demote
             * it. Same DDE race as resched_default and yield_default — demoting
             * act to STATE_READY + LINK3 dispatch exposes a kernel-stack-
             * resident thread to cross-CPU idle_pick steal. Just stash the new
             * priority; the act will see it after its cv_wait body's
             * unready/block point.
             */
            if (act->internal_flags & RTMUX_ITF_WAIT_PENDING) {
                act->priority = (uint8_t) prio;
                break;
            }
            if (sched_defer_displacement(act)) {
                act->priority = (uint8_t) prio;
                _TRACE_TH_EMIT_ANY_STATE(act, act->state);
                break;
            }
            /* Did we lower our prio below another thread which can run? */
            thp = select_thread((int) get_cpunum(), (int) prio);
            if (thp && thp->priority >= prio) {
                /* Another thread is now higher/equal priority. Put active into
                 * ready queue
                 */
                sched_account_expose(); /* single-writer: bill before idle_pick
                                           can steal act
                                           */
                act->state = STATE_READY;
                act->priority = (uint8_t) prio;
                if (priority_inherit) {
                    RR_ADD_PREEMPT_TICK(act);
                    LINK3_BEG(DISPATCH_LST(dpp, act->priority),
                              &act->link3_link, thread_t);
                } else {
                    RR_RESET_TICK(act);
                    LINK3_END(DISPATCH_LST(dpp, act->priority),
                              &act->link3_link, thread_t);
                }

                DISPATCH_SET(dpp, act);

                _TRACE_TH_EMIT_STATE(act, READY);

                mark_running(thp);

                SS_STOP_RUNNING(act, 1);
            } else {
                act->priority = (uint8_t) prio;
                _TRACE_TH_EMIT_ANY_STATE(act, act->state);
            }
        } else {
            act->priority = (uint8_t) prio;
            _TRACE_TH_EMIT_ANY_STATE(act, act->state);
        }
#else
        if (prio < act->priority && !sched_defer_displacement(act) &&
            prio <= DISPATCH_HIGHEST_PRI(dpp)) {
            thread_t *oldact = act;

            /* Put active into ready queue */
            act->priority = (uint8_t) prio;
            sched_account_expose(); /* single-writer: bill before idle_pick can
                                       steal act
                                       */
            act->state = STATE_READY;

            _TRACE_TH_EMIT_STATE(act, READY);
            if (priority_inherit) {
                RR_ADD_PREEMPT_TICK(act);
                LINK3_BEG(DISPATCH_LST(dpp, act->priority), &act->link3_link,
                          thread_t);
            } else {
                RR_RESET_TICK(act);
                LINK3_END(DISPATCH_LST(dpp, act->priority), &act->link3_link,
                          thread_t);
            }

            DISPATCH_SET(dpp, act);

            /* Get highest priority thread. */
            act = DISPATCH_THP(dpp, DISPATCH_HIGHEST_PRI(dpp));
            LINK3_REM(DISPATCH_LST(dpp, act->priority), &act->link3_link,
                      thread_t);
            /* If that list is now empty, clear the readybit.   */
            if (!DISPATCH_THP(dpp, act->priority)) {
                DISPATCH_CLR(dpp, act);
            }

            mark_running(act);

            SS_STOP_RUNNING(oldact, 1);
        } else {
            act->priority = (uint8_t) prio;
            _TRACE_TH_EMIT_ANY_STATE(act, act->state);
        }
#endif
        chk_dispatch(1);
        break;

    case STATE_NET_SEND:
    case STATE_SEND: {
        connect_t *cop;
        uint8_t cur_state;
        channel_t *schp;

        /* Re-check state under the lock. A concurrent force_ready
         * (ksys_connect_detach's EBADF wakeup) or other path that pril_rem's
         * the sender from chp->send_queue can race between the read and our
         * pril_rem.
         *
         * Per-channel lock: snapshot blocked_on to find the channel, lock that
         * chp->lock, then re-verify thp is STILL SEND on THAT channel (state +
         * blocked_on unchanged) before the pril_rem/add; otherwise just adjust
         * priority (the re-sort is best-effort). Locking the snapshot's channel
         * and re-checking blocked_on==cop guarantees we never pril_rem from the
         * wrong channel if thp migrated.
         */
        cop = thp->blocked_on;
        schp = cop ? cop->channel : NULL;
        if (schp) {
            kspinlock_lock(&schp->lock);
            cur_state = thp->state;
            if ((cur_state == STATE_SEND || cur_state == STATE_NET_SEND) &&
                thp->blocked_on == cop) {
                pril_rem(&schp->send_queue, thp);
                thp->priority = (uint8_t) prio;
                pril_add(&schp->send_queue, thp);
            }
            kspinlock_unlock(&schp->lock);
            thp->priority = (uint8_t) prio;
        } else {
            thp->priority = (uint8_t) prio;
        }
        break;
    }

    /* STATE_MUTEX, STATE_CONDVAR and STATE_SEM waiters sit on a sync chain, not
     * on a ksync_t list (blocked_on is NULL or stale for them): the new
     * priority is stored here and prio_apply repositions them on the chain.
     */
    default:
        thp->priority = (uint8_t) prio;
    }
    chk_dispatch(1);
    SCHED_UNLOCK(outer_dpp);
}

int rdecl stop_one_thread(process_t *prp,
                          int tid,
                          unsigned flags,
                          unsigned iflags)
{
    thread_t *thp, *thp2;


    if ((tid < prp->threads.nentries) &&
        (thp = VECP2(thp2, &prp->threads, tid))) {
        if ((thp == thp2) || (thp->flags & RTMUX_FLG_THR_WAAA)) {
            atomic_or(&thp->flags, RTMUX_FLG_THR_TO_BE_STOPPED | flags);
            ITF_SET(thp, iflags);
#if CONFIG_PROCESSORS_MAX > 1
            if (thp->state == STATE_RUNNING &&
                thp != get_active_thread(get_cpunum())) {
                SENDIPI(thp->runcpu, IPI_RESCHED);
            }
#endif
            return 1;
        }
    }
    return 0;
}

void rdecl stop_threads(process_t *prp, unsigned flags, unsigned iflags)
{
    int tid;

    for (tid = 0; tid < prp->threads.nentries; ++tid) {
        stop_one_thread(prp, tid, flags, iflags);
    }
}


int rdecl cont_one_thread(process_t *prp, int tid, unsigned flags)
{
    thread_t *thp, *thp2;

    if ((tid < prp->threads.nentries) &&
        (thp = VECP2(thp2, &prp->threads, tid))) {
        if ((thp == thp2) || (thp->flags & RTMUX_FLG_THR_WAAA)) {
            if ((prp->flags &
                 (RTMUX_FLG_PROC_STOPPED | RTMUX_FLG_PROC_DEBUG_STOPPED |
                  RTMUX_FLG_PROC_COREDUMP)) == 0) {
                atomic_clr(&thp->flags, flags);
                if ((thp->flags & (RTMUX_FLG_THR_FROZEN |
                                   RTMUX_FLG_THR_THREADS_HOLD)) == 0) {
                    atomic_clr(&thp->flags, RTMUX_FLG_THR_TO_BE_STOPPED);
                    if (thp->state == STATE_STOPPED) {
                        ready(thp);
                    }
                }
            }
            return 1;
        }
    }
    return 0;
}


void rdecl cont_threads(process_t *prp, unsigned flags)
{
    int tid;

    for (tid = 0; tid < prp->threads.nentries; ++tid) {
        cont_one_thread(prp, tid, flags);
    }
}

/* Set the runmask for the calling thread. */
int rdecl set_runmask(thread_t *thp, unsigned runmask)
{
    thread_t *act = get_active_thread(get_cpunum());

    /* Must specify at least one available processor. */
    if ((runmask & LEGAL_CPU_BITMASK) == 0) {
        kcall_err(act, EINVAL);
        return -1;
    }

    /* We keep an inverted copy of the mask internally. */
    thp->runmask = ~runmask;

#if CONFIG_PROCESSORS_MAX > 1
    /* If we are on a CPU which we have masked then force us off! */
    if (thp->runmask & (1u << thp->runcpu)) {
        if (thp == act) {
            /* Move off this hart: block() hands it to another thread and
             * ready() queues act on a hart its runmask allows. act stays
             * STATE_RUNNING throughout, so SIGCONT, a debugger resume or
             * teardown only flag and kick it and never ready it a second time.
             * block() updates timestamp_last_block and charges the sporadic
             * budget when it hands the hart to another thread (not when the
             * hart goes idle, as for any block). ready_default skips the new
             * sporadic activation for a RUNNING thread, so start it here, as a
             * block and ready through any other state would.
             */
            block();

            /* Test the policy again under the lock: block() may have passed an
             * RCU quiescent state, and a policy change frees ss_info after one.
             */
            if (IS_SCHED_SS(thp)) {
                SCHED_LOCK_LOCAL();
                if (IS_SCHED_SS(thp))
                    sched_ss_activate(thp);
                SCHED_UNLOCK_LOCAL();
            }
            ready(thp);
        } else if (thp->state == STATE_RUNNING) {
            SENDIPI(thp->runcpu, IPI_RESCHED);
        }
    }
#endif

    return 0;
}

/* in priority-based sheduling, a thread may always run, if it's got the
 * priority
 */
static int rdecl may_thread_run_default(thread_t *thp)
{
    return 1;
}

/* sched_trace_initial_parms_default — hook target for
 * sched_trace_initial_parms; no global parms need tracing.
 */
static void sched_trace_initial_parms_default() {}


static void rdecl debug_moduleinfo_default(process_t *prp,
                                           thread_t *thp,
                                           void *dbg)
{
    if (!thp) { /* process_t-level information */
        memset(dbg, 0, sizeof(((debug_process_t *) NULL)->extsched));
    } else { /* thread_t-level information */
        memset(dbg, 0, sizeof(((debug_thread_t *) NULL)->extsched));
    }
}

/**
 * Initialize the scheduler callbacks
 */

kdispatch_t *init_scheduler_default(void)
{
    /* Init function pointers */
    ready = ready_default;
    block_and_ready = block_and_ready_default;
    select_thread = select_thread_default;
    mark_running = mark_running_default;
    adjust_priority = adjust_priority_default;
    resched = resched_default;
    yield = yield_default;
    may_thread_run = may_thread_run_default;
    sched_trace_initial_parms = sched_trace_initial_parms_default;
    debug_moduleinfo = debug_moduleinfo_default;

    /* Allocate a kdispatch_t per active hart; each instance owns its ready[]
     * queues, priority bitmap, AND its own kspinlock_t lock. Same-CPU dispatch
     * operations hold only the local instance's lock; cross-CPU operations take
     * the target's lock; multi-CPU sequences acquire locks in ascending-address
     * (= ascending hart_id) order to avoid deadlock.
     */
    for (int idx = 0; idx < (int) num_processors; idx++) {
        unsigned hart = (unsigned) idx;
        pcpu_array[hart].pc_dispatch = _scalloc(sizeof(kdispatch_t));
        kspinlock_init(&pcpu_array[hart].pc_dispatch->lock, LOCK_CLASS_SCHED,
                       "sched_dpp");
    }
    scheduler_type = SCHEDULER_TYPE_DEFAULT;

    /* Return the boot-CPU's instance as the canonical "default dispatch" —
     * kernel_threads_prp->default_dpp inherits this so downstream
     * pointer-equality checks (e.g. fastmsg.c's "is thp on the default group"
     * check) still hold for the boot CPU. Threads migrating to other CPUs get
     * thp->dpp updated to the target hart's instance; the per-CPU instances all
     * belong to the same logical "default" group.
     */
    return pcpu_array[boot_cpunum].pc_dispatch;
}
