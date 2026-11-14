/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <lock.h>
#include <stdatomic.h>

/** Notes about the interlocking in this file.
 *
 * The active timer list(s) can be examined/manipulated by two environments -
 * the interrupt subsystem (via timer_expiry()) and normal kernel call
 * operations. Both now serialize on a single lock, timer_wheel_slock, which
 * owns the entire wheel (mon_timers/tod_timers slot lists, num_active counts,
 * the queue chain, and queue_dead). Every mutator and reader of the wheel
 * (timer_insert / timer_rearm / timer_activate / timer_deactivate /
 * timer_period / timer_next / timer_pending) takes it, and so does the
 * timer_expiry() queue walk. The lock is IRQ-disabling (kspinlock_lock), so the
 * same-hart clock IRQ cannot fire while a mutator holds it.
 *
 * The lock is NOT recursive. sigevent delivery must NEVER happen while it is
 * held: sigevent_deliver_inline -> sigevent_exe -> ready -> timeout_stop ->
 * timer_deactivate re-enters and would take the same lock on the same hart,
 * self-deadlocking. So the two delivery sites (timer_expiry firing a batch,
 * timer_activate firing a past-deadline absolute timer) do the wheel work under
 * the lock, RELEASE, and only then deliver.
 *
 * This replaced the old lockless per-timer-op / pending-list scheme: a single
 * volatile flag could make the ISR defer linkage edits to a pending list, but
 * it did NOT stop a second hart's timer_activate/timer_deactivate from mutating
 * the same slot list concurrently once the BKL was gone, which tripped
 * CHECK_LINKAGES under SMP. The lock subsumes all of it.
 */

enum fire_source {
    FS_INTR,
    FS_ACTIVATE,
};

#define TQ_FLAG_TOD 0x0001
#define TQ_FLAG_OLD 0x0002

struct timer_queue {
    struct timer_queue *next;
    unsigned downshift;
    unsigned mask;
    unsigned num_active;
    unsigned flags;
    unsigned fire_idx;
    unsigned last_tick_idx;
    uint64_t last_tick; /* absolute tick (time >> downshift) of the last walk */
    struct timer_link slot[1]; /* variable sized */
};

/* timer_ops_delayed — externally visible so kernel_exit (arch/riscv/exit.c) and
 * cpu_idle can read it to decide whether to call timer_pending(). timer_expiry
 * sets it to 1 when retired timer queues (queue_dead) need freeing;
 * timer_pending() clears it at the top of its body. Freeing must happen OUTSIDE
 * the trap/expiry context (it may free memory and takes the wheel lock
 * unnested), hence the deferral.
 */
volatile uint8_t timer_ops_delayed;

/* timer_drain_busy — CAS-claim ensuring at most ONE hart enters timer_pending()
 * at a time across all harts that see timer_ops_delayed == 1. Without this
 * gate, every idle hart on a busy system races into timer_pending and
 * serializes on the wheel lock for the dead-queue sweep.
 *
 * Consumers (kernel_exit, cpu_idle) do:
 *
 *     int expected = 0;
 *     if (timer_ops_delayed &&
 *         atomic_compare_exchange_strong(&timer_drain_busy, &expected, 1)) {
 *         timer_pending();            // clears timer_ops_delayed
 *         atomic_store(&timer_drain_busy, 0);
 *     }
 *
 * Hart-ID-agnostic: whichever hart wins the CAS does the drain. No assumption
 * about a "boot CPU" or "hart 0" being available — RTMux's hart IDs are
 * non-contiguous on real hardware (per get_cpunum()'s doc in runtime.h).
 *
 * If new timer work arrives mid-drain, the producer sets timer_ops_delayed back
 * to 1; the next idle hart pass picks it up.
 */
atomic_int timer_drain_busy;

/* timer_wheel_slock — the single lock that owns the entire timer wheel. See the
 * interlocking note at the top of this file. IRQ-disabling and nest-aware
 * (kspinlock_lock) but NOT recursive, so never re-take it on the same hart and
 * never hold it across sigevent delivery.
 */
static kspinlock_t timer_wheel_slock;

/* timer_backlog — set by timer_expiry() when it hit the per-interrupt fire cap
 * with timers still due, meaning more expiry work is pending.
 * arch_timer_program_next() reads it (boot CPU only, same as the producer) and
 * shortens the next interrupt to TIMER_BACKLOG_DELTA_NS instead of a full
 * nsec_inc tick, so a burst of due timers drains over several quick bounded
 * ISRs rather than one fat one (keeps the 50-fire latency bound) or one per
 * slow tick (the old throughput ceiling). The delta is bounded well above the
 * ~1-cycle livelock window, so re-firing while a kernel call is mid-flight
 * still lets that call advance.
 */
volatile uint8_t timer_backlog;

static struct timer_queue *queue_head;
static struct timer_queue *queue_dead;
static struct timer_queue *mon_timers; /* Ptr to active timers (relative). */
static struct timer_queue *tod_timers; /* Ptr to active timers (absolute). */
static _Atomic uint64_t cached_next_deadline = ~(uint64_t) 0;

/* We can get away with only downshifting the low order 32 bits of the 64-bit
 * nanosecond value because the downshift and mask bits will never select
 * anything in the upper 32 bits.
 */
#define GET_QUEUE_IDX(q, t) (((((uint32_t) (t)) >> (q)->downshift)) & (q)->mask)

#define ADD_IN_NEW_QUEUE(head, old, new) \
    do {                                 \
        (new)->flags = (old)->flags;     \
        (new)->next = (old)->next;       \
        (old)->next = (new);             \
        MEM_BARRIER_WR();                \
        *(head) = (new);                 \
        (old)->flags |= TQ_FLAG_OLD;     \
    } while (0)

/* Consistency check that the forward and backward links are sane */
#define CHECK_LINKAGES(tip)                           \
    CRASHCHECK((tip)->link.prev->link.next != (tip)); \
    CRASHCHECK((tip)->link.next->link.prev != (tip));

/* Normalize a timer's fire time to the monotonic timebase used by the hardware
 * rearm path (arch_timer_now_ns). Monotonic timers already store a monotonic
 * deadline; TOD-based (CLOCK_REALTIME) timers store an absolute TOD deadline,
 * so subtract the current TOD adjustment to convert it. Without this the cache
 * would compare TOD-absolute and monotonic deadlines against a monotonic "now"
 * - the same normalization timer_next() already does.
 */
static uint64_t timer_deadline_mono(const ktimer_t *tip)
{
    uint64_t deadline = tip->itime.nsec;

    if (tip->flags & RTMUX_TI_TOD_BASED) {
        uint64_t adj = qtimeptr->nsec_tod_adjust;
        deadline = deadline > adj ? deadline - adj : 0;
    }
    return deadline;
}

/* How far behind a late interval timer may fall and still fire every missed
 * period. The shortest interval is one tick (1 ms), so this caps the burst at
 * about a thousand fires.
 */
#define TIMER_CATCHUP_NS 1000000000ULL

/* The wheel's current time in tip's timebase: what timer_expiry compares the
 * deadline against (qtp->nsec, plus the TOD adjustment for TOD-based timers).
 */
static uint64_t timer_wheel_now(const ktimer_t *tip)
{
    uint64_t now = qtimeptr->nsec;

    if (tip->flags & RTMUX_TI_TOD_BASED)
        now += qtimeptr->nsec_tod_adjust;
    return now;
}

static void timer_note_deadline(ktimer_t *tip)
{
    uint64_t deadline, cur;

    if (tip->clockid == CLOCK_SOFTTIME || tip->pending)
        return;
    deadline = timer_deadline_mono(tip);

    /* Atomic min into cached_next_deadline. A plain load/compare/store would
     * let two CPUs each arming an earlier timer lose the true minimum (the
     * larger store winning), arming the boot CPU late; the CAS loop forbids it.
     */
    cur = atomic_load_explicit(&cached_next_deadline, memory_order_relaxed);
    do {
        if (deadline >= cur)
            return; /* not the new earliest, nothing to reprogram */
    } while (!atomic_compare_exchange_weak_explicit(
        &cached_next_deadline, &cur, deadline, memory_order_relaxed,
        memory_order_relaxed));

    /* We lowered the cached deadline. The boot CPU owns the hardware timer and
     * trims it down to cached_next_deadline (see arch_timer_program_next); if
     * this ran on another CPU, kick the boot CPU so it reprograms instead of
     * sleeping until its stale, later deadline. On a UP build SENDIPI compiles
     * away.
     */
    if (get_cpunum() == boot_cpunum)
        (void) arch_timer_reload(qtimeptr);
    else
        SENDIPI(boot_cpunum, IPI_CLOCK_LOAD);
}

static void timer_recompute_deadline(void)
{
    struct timer_queue *queue;
    ktimer_t *tip;
    ktimer_t *first;
    unsigned i;
    uint64_t next = ~(uint64_t) 0;

    for (queue = queue_head; queue; queue = queue->next) {
        if (queue->num_active == 0)
            continue;
        for (i = 0; i <= queue->mask; i++) {
            first = (ktimer_t *) &queue->slot[i];
            for (tip = first->link.next; tip != first; tip = tip->link.next) {
                if (tip->clockid != CLOCK_SOFTTIME && !tip->pending) {
                    uint64_t deadline = timer_deadline_mono(tip);
                    if (deadline < next)
                        next = deadline;
                }
            }
        }
    }
    atomic_store_explicit(&cached_next_deadline, next, memory_order_relaxed);
}

void rdecl timer_next_deadline(uint64_t *nexttime)
{
    *nexttime =
        atomic_load_explicit(&cached_next_deadline, memory_order_relaxed);
}

/* Drop the cached earliest deadline back to "none". The hardware rearm path
 * only ever trims the next interrupt down to nsec_inc, never beyond it, so the
 * boot CPU still wakes within one tick and rebuilds the cache (via
 * timer_note_deadline on the next arm, or timer_recompute_deadline at the next
 * expiry). Used where the precise next deadline cannot be recomputed safely,
 * such as a TOD step where clock_settime shifts every TOD timer's monotonic
 * deadline.
 */
void rdecl timer_deadline_invalidate(void)
{
    atomic_store_explicit(&cached_next_deadline, ~(uint64_t) 0,
                          memory_order_relaxed);
}

/* Per-CPU "fired-but-undelivered" buffer.
 *
 * timer_fire records (event, thread) pairs into this buffer instead of calling
 * sigevent_deliver_inline directly; timer_expiry walks the buffer AFTER it
 * releases timer_wheel_slock and delivers each pair.
 *
 * Why: sigevent_deliver_inline → sigevent_exe → ready/force_ready may call
 * timeout_stop → timer_deactivate, which takes timer_wheel_slock. Our own
 * timer_expiry holds that lock across the whole queue walk; delivering inline
 * would re-enter the non-recursive lock on the same hart and self-deadlock.
 * Deferring delivery until after the unlock breaks the cycle.
 *
 * Bounded by the nfires limit (50 per timer_expiry call); 64 slots per CPU is
 * the rounded-up max. CONFIG_PROCESSORS_MAX is always >= 1, so this dimension
 * covers both the UP and SMP builds.
 */
#define TIMER_FIRE_DEFER_MAX 64
struct timer_fire_defer {
    struct sigevent event;
    thread_t *thp;
    uint8_t
        reserved; /* tip->nresv snapshot: allocation-free reserve delivery */
};
static struct timer_fire_defer timer_fire_deferred[CONFIG_PROCESSORS_MAX]
                                                  [TIMER_FIRE_DEFER_MAX];
static unsigned timer_fire_deferred_n[CONFIG_PROCESSORS_MAX];

/* Wait until no CPU has an undrained timer_fire_deferred[] batch.
 *
 * timer_expiry records a fire by value (including its reserved flag) under the
 * wheel lock, releases the lock, then drains the batch on the same hart. A
 * reserved fire of a timer being destroyed can therefore still be pending in
 * another hart's buffer after timer_free's wheel-lock barrier. Releasing that
 * timer's notification-soul reservation before the pending fire drains would
 * let non-RT allocation and the pending reserved pop together starve a live
 * source's reserved delivery. timer_free calls this before object_unreserve so
 * the reservation outlives the last by-value fire (the timer-side analog of
 * intrevent_flush for the IRQ path). A destroyed timer is already off the
 * wheel, so it can never be re-recorded; the wait is bounded by the in-progress
 * drains. Single-hart builds drain within the same trap, so there is nothing to
 * wait for.
 */
static void timer_fire_deferred_flush(void)
{
#if CONFIG_PROCESSORS_MAX > 1
    for (;;) {
        bool pending = false;
        unsigned i;

        MEM_BARRIER_RW();
        for (i = 0; i < CONFIG_PROCESSORS_MAX; i++) {
            if (timer_fire_deferred_n[i]) {
                pending = true;
                break;
            }
        }
        if (!pending)
            break;
    }
#endif
}

/* Per-CPU deferred timeout fire. timer_activate(..., defer=1) — the
 * block-timeout arming path — captures a past-deadline timeout's (event,
 * thread) here instead of delivering it inline, because block_release() arms
 * the timeout while still holding the turnstile/sync-chain lock that
 * force_ready would re-acquire on the same hart (non-recursive spinlock →
 * self-deadlock; observed under SMP load when the deadline is already past at
 * arm time). block_release() drains it via timeout_deferred_fire() AFTER it
 * releases that lock. At most one is pending per CPU: timeout_start arms once
 * per kernel call (RTMUX_FLG_TIMEOUT_ACTIVE guard) and the same kernel call
 * drains it before any context switch. The IPC send/reply/receive and join
 * paths also defer (WAIT_PENDING set: force_ready would spin on the thread's
 * own bit) and drain right after clearing it.
 */
struct timeout_fire_defer {
    struct sigevent event;
    thread_t *thp;
    uint8_t
        reserved; /* tip->nresv snapshot: allocation-free reserve delivery */
    uint8_t valid;
};
static struct timeout_fire_defer timeout_fire_pending[CONFIG_PROCESSORS_MAX];

/* The single wheel lock subsumes the old per-op/TICKER coordination. Every site
 * that used to bracket wheel access with WHEEL_LOCK()/WHEEL_UNLOCK() now
 * takes/releases timer_wheel_slock. kspinlock_lock disables IRQs, so the
 * same-hart clock interrupt cannot fire mid-mutation.
 */
#define WHEEL_LOCK() kspinlock_lock(&timer_wheel_slock)
#define WHEEL_UNLOCK() kspinlock_unlock(&timer_wheel_slock)


/* Allocate and initialize a timer queue with empty slot lists. Parameters:
 *   entries   : number of slots (must be a power of two)
 *   downshift : right-shift applied to nsec to index the slot array
 * Return value: the new queue, or NULL on allocation failure.
 */
static struct timer_queue *new_queue(unsigned entries, unsigned downshift)
{
    struct timer_queue *new;
    struct timer_link *link;

    new = _scalloc(sizeof(*new) + (entries - 1) * sizeof(new->slot[0]));
    if (new) {
        new->mask = entries - 1;
        new->downshift = downshift;
        do {
            link = &new->slot[--entries];
            link->prev = link->next = (ktimer_t *) link;
        } while (entries != 0);
    }
    return new;
}


/* Free a retired timer queue. Parameters:
 *   queue : queue to free (must not be the live mon/tod head)
 */
static void kill_queue(struct timer_queue *queue)
{
    CRASHCHECK(queue == mon_timers || queue == tod_timers);
    _sfree(queue, sizeof(*queue) + (queue->mask) * sizeof(queue->slot[0]));
}


/* Insert a timer into its (monotonic or TOD) timer queue.
 *
 * Finds the time-ordered insertion point in the target slot. Parameters:
 *   tip : timer to insert (its itime.nsec is the fire time)
 *   src : caller context (FS_INTR / FS_ACTIVATE)
 */
static void timer_insert(ktimer_t *tip, enum fire_source src)
{
    ktimer_t *fore;
    ktimer_t *back;
    struct timer_queue **head;
    struct timer_queue *queue;
    unsigned idx;

    if (tip->flags & RTMUX_TI_TOD_BASED) {
        head = &tod_timers;
    } else {
        head = &mon_timers;
    }
    queue = *head;

    tip->queue = queue;

    (void) src;

    /* No insert-time growth. Dropping the wheel lock here would need ktimer
     * lifetime refs; keep correctness and accept longer slot walks.
     */
    idx = GET_QUEUE_IDX(queue, tip->itime.nsec);

    /* A deadline at or before the wheel's current time hashes to a slot the
     * expiry walk may already have passed (it visits fire_idx up to the current
     * tick), and would then wait for the ring to come round again. Meanwhile
     * the cached deadline stays overdue and an idle hart re-arms every
     * TIMER_BACKLOG_DELTA_NS for it: a periodic 50 ms sleeper kept an idle hart
     * at hundreds of interrupts a second. Put it in the slot the next walk
     * starts from instead; the slot is sorted, so it fires first.
     */
    if (tip->itime.nsec <= timer_wheel_now(tip))
        idx = queue->fire_idx;
    back = fore = (ktimer_t *) &queue->slot[idx];
    if (fore->link.next != back) {
        /* Not an empty list - search backwards and forwards */
        for (;;) {
            CHECK_LINKAGES(fore);
            CHECK_LINKAGES(back);
            fore = fore->link.next;
            back = back->link.prev;
            if (tip->itime.nsec >= back->itime.nsec)
                goto insert_after;
            if (tip->itime.nsec < fore->itime.nsec)
                goto insert_before;
        }
    }
    /* Link the timer in, careful about order - timer_expiry could be running */

insert_after:
    /* insert after the test point */
    tip->link.next = back->link.next;
    tip->link.prev = back;
    back->link.next->link.prev = tip;
    back->link.next = tip;
    CHECK_LINKAGES(tip);
    CHECK_LINKAGES(back);
    queue->num_active += 1;
    timer_note_deadline(tip);
    return;

insert_before:
    /* insert before the test point */
    tip->link.prev = fore->link.prev;
    tip->link.next = fore;
    fore->link.prev->link.next = tip;
    fore->link.prev = tip;
    CHECK_LINKAGES(tip);
    CHECK_LINKAGES(fore);
    queue->num_active += 1;
    timer_note_deadline(tip);
    return;
}


/* Re-arm or retire a timer after it fires.
 *
 * Unlinks the timer from its queue (unless called from FS_ACTIVATE), clears the
 * pending indicator, and either marks it expired (one-shot) or recomputes the
 * next fire time and re-inserts it (interval timer). Parameters:
 *   tip : timer to re-arm
 *   src : caller context (FS_INTR / FS_ACTIVATE)
 */
static void timer_rearm(ktimer_t *tip, enum fire_source src)
{
    uint64_t interval;
    systime_t *qtp;

    /* On SMP, timer_deactivate on another CPU may have already cleared
     * RTMUX_TI_ACTIVE. Just bail — the timer is dead.
     */
    if (!(tip->flags & RTMUX_TI_ACTIVE))
        return;
    if (src != FS_ACTIVATE) {
        /* Unlink from the active list. timer_wheel_slock is held by the caller,
         * so no other hart can be touching this slot list. tip->queue is still
         * read once for the num_active bookkeeping.
         */
        struct timer_queue *q = tip->queue;
        if (!q)
            return;
        tip->link.next->link.prev = tip->link.prev;
        tip->link.prev->link.next = tip->link.next;
        CHECK_LINKAGES(tip->link.prev);
        CHECK_LINKAGES(tip->link.next);
        q->num_active -= 1;
    }

    /* Off the active list now. pending was a relic of the lockless ISR-defer
     * scheme; keep it NULL so any stale reader treats the timer as armable.
     */
    tip->pending = NULL;

    if (tip->itime.interval_nsec == 0) {
        /* Timer expired normally */
        tip->flags =
            (uint8_t) ((tip->flags & ~RTMUX_TI_ACTIVE) | RTMUX_TI_EXPIRED);
        tip->queue = NULL;
        return;
    }
    /* An interval timer, so we must reactivate it with a new time. */

    qtp = qtimeptr;
    /* The following check prevents infinite loops. */
    interval = tip->itime.interval_nsec;
    if (interval < qtp->nsec_inc) {
        interval = qtp->nsec_inc;
    }
    /* Recalculate the trigger time. */
    tip->itime.nsec += interval;

    /* Still behind after one interval. CLOCK_SOFTTIME resyncs to now (PR-8735).
     * Every other clock keeps its missed periods, which timer_insert fires back
     * to back from the current slot: callers that count ticks (the LKL
     * measurement timers) rely on each one, and stall if any are skipped, even
     * under heavy load. Only a timer more than TIMER_CATCHUP_NS behind (a start
     * time far in the past, a large forward TOD step) skips to the present,
     * counting the skipped periods in overruns, so it cannot keep the boot hart
     * firing it for seconds.
     */
    if (tip->clockid == CLOCK_SOFTTIME) {
        uint64_t tod;

        snap_time(&tod, tip->flags & RTMUX_TI_TOD_BASED);
        if (tip->itime.nsec < tod) {
            tip->itime.nsec = tod + interval;
        }
    } else {
        uint64_t now = timer_wheel_now(tip);

        if (tip->itime.nsec <= now && now - tip->itime.nsec > TIMER_CATCHUP_NS) {
            uint64_t missed = (now - tip->itime.nsec) / interval + 1;

            tip->itime.nsec += missed * interval;
            tip->overruns = missed > UINT32_MAX - tip->overruns
                                ? UINT32_MAX
                                : tip->overruns + (uint32_t) missed;
        }
    }
    timer_insert(tip, src);
}


/* Fire a single expired timer.
 *
 * Records the timer's (event, thread) pair into this CPU's deferred-delivery
 * buffer (drained after the wheel lock is released), then re-arms it inline.
 * Parameters:
 *   tip : the expired timer
 * Return value: the next timer to examine in the slot list.
 */
static ktimer_t *timer_fire(ktimer_t *tip)
{
    thread_t *thp;
    ktimer_t *next;

    next = tip->link.next;
    CRASHCHECK(!next);
    if (tip->pending || !tip->queue) {
        /* Already fired or being deleted */
        return next;
    }
    CRASHCHECK(!(tip->flags & RTMUX_TI_ACTIVE));
    CRASHCHECK(!tip->link.prev);

    /* timer_wheel_slock is held across the whole expiry walk, so no mutator on
     * another hart can be editing this slot list; the linkages are sane and we
     * re-arm directly below. No more pending-list defer.
     */
    CHECK_LINKAGES(tip);

    thp = tip->thread;

    /* Try and act on the event associated with this timer. Note: In the case of
     * a timeout this may release tip
     */
    if (thp) {
        /* Direct sigevent delivery, deferred-inside-trap variant. RECORD the
         * (event, thread) pair into this CPU's timer_fire_deferred[] buffer
         * instead of calling sigevent_deliver_inline directly. timer_expiry
         * drains the buffer AFTER it releases timer_wheel_slock, by which point
         * the recursion cycle (sigevent_exe → ready → timeout_stop →
         * timer_deactivate → WHEEL_LOCK → kspinlock_lock) no longer re-enters
         * the lock our own walk held — the lock is released before any drain.
         *
         * The buffer cannot overflow: timer_expiry stops the walk at the
         * 50-fire cap (goto capped_out) without overshooting, so at most 51
         * pairs are recorded per expiry, well under TIMER_FIRE_DEFER_MAX (64).
         * The slot < TIMER_FIRE_DEFER_MAX guard below is a belt-and-suspenders
         * bound, not a normal drop path (a dropped one-shot fire would be lost
         * — timer_rearm removes it from the wheel regardless of delivery).
         *
         * Note: the sigevent is copied BY VALUE into the deferred buffer, not
         * recorded as a &tip->event pointer. A concurrent timer_destroy →
         * timer_free → object_free(tip) on another hart can run between the
         * WHEEL_UNLOCK() that ends the walk and the post-unlock drain; a
         * pointer into tip would be a use-after-free. The by-value copy makes
         * each deferred entry self-contained, so the drain never dereferences
         * tip.
         */
        unsigned cpu = get_cpunum();
        unsigned slot = timer_fire_deferred_n[cpu];
        if (slot < TIMER_FIRE_DEFER_MAX) {
            timer_fire_deferred[cpu][slot].event = tip->event;
            timer_fire_deferred[cpu][slot].thp = thp;
            timer_fire_deferred[cpu][slot].reserved = tip->nresv;
            timer_fire_deferred_n[cpu] = slot + 1;
        }
    } else {
        /* thread has gone away, deactivate the timer */
        tip->itime.interval_nsec = 0;
    }

    /* We have to check for tip->queue again, even though we did it at the top
     * of the function, because we might have done a sigevent_exe() on a
     * __timer_timeout timer and the code would have called timer_deactivate()
     * after delivering the event.
     */
    if (!tip->pending && tip->queue) {
        ktimer_t *prev = tip->link.prev;

        timer_rearm(tip, FS_INTR);
        if (prev->link.next == tip) {
            /* It's been re-armed in front of the next entry, so we have to
             * check it again.
             */
            next = tip;
        }
    }
    return next;
}


/* Fire all timers due as of the current time.
 *
 * Walks each timer queue's slots up to the current tick, firing expired timers
 * (capped at 50 per call), reaping retired queues, then releases the wheel lock
 * and delivers the deferred (event, thread) pairs. Sets timer_ops_delayed if
 * retired queues still need freeing. Parameters:
 *   qtp : current qtime (provides nsec and TOD adjustment)
 */
void rdecl timer_expiry(systime_t *qtp)
{
    uint64_t tod = qtp->nsec;
    struct timer_queue *queue;
    struct timer_queue **owner;

    /* Reset this CPU's deferred-deliver buffer before the walk; populated by
     * timer_fire(), drained after the wheel lock is released.
     */
    timer_fire_deferred_n[get_cpunum()] = 0;
    ktimer_t *tip;
    ktimer_t *first;
    unsigned nfires;
    unsigned curr_tick_idx;
    uint64_t now_tick;
    unsigned count;
    int adjusted = 0;
    int capped = 0;

    nfires = 0;

    /* Own the wheel for the entire walk + rearm + recompute. Release it before
     * draining the deferred sigevents (delivery re-enters the wheel lock via
     * timeout_stop → timer_deactivate, and the lock is not recursive).
     *
     * timer_expiry runs in the clock trap with SIE already off. Use the
     * IRQ-state-preserving lock pair so the outermost unlock does NOT re-enable
     * SIE: the post-unlock deferred-deliver drain (and the rest of the ISR)
     * must keep running with interrupts disabled. Re-enabling SIE here would
     * let a nested clock IRQ re-enter timer_expiry and wipe this frame's
     * timer_fire_deferred_n[] before we drain it.
     */
    unsigned long sie = kspinlock_lock_irqsave(&timer_wheel_slock);
    owner = &queue_head;
    for (;;) {
        queue = *owner;
        if (!queue)
            break;
        if (!adjusted && (queue->flags & TQ_FLAG_TOD)) {
            adjusted = 1;
            tod += qtp->nsec_tod_adjust;
        }
        curr_tick_idx = GET_QUEUE_IDX(queue, tod);
        now_tick = tod >> queue->downshift;
        if (queue->num_active != 0) {
            owner = &queue->next;

            /* Slots to visit: those the last walk left behind (a capped walk
             * stops short of last_tick_idx) plus the ticks since it ran. Count
             * the elapsed ticks from absolute tick numbers, not slot indices: a
             * tickless hart can sleep a whole number of turns of the wheel
             * (four 1 ms slots for the relative queue), and the index
             * difference then reads as no time at all, so the walk skipped
             * every slot but the current one and a due timer waited a turn.
             */
            count = (queue->last_tick_idx - queue->fire_idx) & queue->mask;
            if (now_tick - queue->last_tick > queue->mask)
                count = queue->mask + 1;
            else
                count += (unsigned) (now_tick - queue->last_tick);
            if (count > queue->mask) {
                /* We've wrapped around, have to examine all the slots */
                queue->fire_idx = (curr_tick_idx + 1) & queue->mask;
            }
            for (;;) {
                first = (ktimer_t *) &queue->slot[queue->fire_idx];
                tip = first->link.next;
                for (;;) {
                    if (tip == first)
                        break;
                    CRASHCHECK((tip->queue != queue) && (tip->queue));
                    if (tip->itime.nsec > tod)
                        break;

                    tip = timer_fire(tip);

                    /* - Only fire up to 50 timers in one interrupt to minimize
                     *   latency.
                     * - If a higher-priority wakeup was already produced
                     *   in this trap, kick out so dispatch gets a chance.
                     */

                    if ((++nfires > 50) ||
                        (pcpu_array[get_cpunum()].pc_pending_preempt_prio >=
                         NUM_PRI - 1)) {
                        /* Cap hit: stop the WHOLE walk now, leaving fire_idx at
                         * this unfinished slot so the timers we did not reach
                         * stay in the wheel and fire on the next expiry (the
                         * backlog flag set below shortens that to ~100us). Only
                         * breaking the inner loop would let the slot loop keep
                         * firing one timer per remaining slot — overshooting
                         * the 50 bound far past the 64-entry deferred buffer,
                         * whose dropped fires are NOT re-armed and so would be
                         * lost.
                         */
                        capped = 1;
                        goto capped_out;
                    }
                }
                if (queue->fire_idx == curr_tick_idx)
                    break;
                queue->fire_idx = (queue->fire_idx + 1) & queue->mask;
            }
        } else if (queue->flags & TQ_FLAG_OLD) {
            /* Idle, retired queue. We hold timer_wheel_slock, so it is safe to
             * unchain it from the live list now; the actual _sfree happens in
             * timer_pending() outside trap context (it may free memory).
             */
            *owner = queue->next;
            queue->next = queue_dead;
            queue_dead = queue;
        } else {
            owner = &queue->next;
            queue->fire_idx = curr_tick_idx;
        }
        queue->last_tick_idx = curr_tick_idx;
        queue->last_tick = now_tick;
    }
capped_out:
    /* Recompute the cached earliest deadline while we still own the wheel: the
     * lock makes this queue walk safe against a concurrent
     * timer_activate()/timer_deactivate(). Must run before WHEEL_UNLOCK(). On
     * the capped path fire_idx still points at the unfinished slot, so the
     * cached minimum (and next interrupt) reflects the timers still due there.
     */
    if (nfires != 0)
        timer_recompute_deadline();

    /* Tell arch_timer_program_next there is a backlog so it shortens the next
     * interrupt; cleared once a walk drains without hitting the cap.
     */
    timer_backlog = (uint8_t) capped;
    kspinlock_unlock_irqrestore(&timer_wheel_slock, sie);

    /* Drain the deferred-deliver buffer NOW that the wheel lock is released.
     * Any recursion via sigevent_exe → ready → timeout_stop → timer_deactivate
     * → WHEEL_LOCK takes the lock cleanly because we no longer hold it. This is
     * what makes the inline-from-trap delivery model safe for the timer path.
     */
    {
        unsigned cpu = get_cpunum();
        unsigned n = timer_fire_deferred_n[cpu];
        unsigned i;
        for (i = 0; i < n; i++) {
            sigevent_deliver_inline(&timer_fire_deferred[cpu][i].event,
                                    timer_fire_deferred[cpu][i].thp,
                                    timer_fire_deferred[cpu][i].reserved);
        }
        timer_fire_deferred_n[cpu] = 0;
    }

    if (!timer_ops_delayed && (queue_dead)) {
        /* Retired timer queues need freeing, but timer_pending() must NOT run
         * inline from this trap-time context (clock_handler runs in a trap with
         * INKERNEL_NOW unset), so set the flag and let the next kernel_exit /
         * cpu_idle on any hart drain it.
         */
        timer_ops_delayed = 1;
    }
}


/* Free the timer queues retired by timer_expiry().
 *
 * With the wheel owned by a single lock, timers are always re-armed inline
 * during expiry, so there is no longer a pending re-arm list to process. The
 * only deferred work left is freeing the queue structs that timer_expiry
 * unchained into queue_dead — that _sfree must happen outside trap/expiry
 * context (it may free memory). Must NOT be called with timer_wheel_slock held.
 */
void rdecl timer_pending(void)
{
    struct timer_queue *queue;
    struct timer_queue *next;

    /* Mark deferred work as claimed before we detach the dead list, so a
     * producer that retires another queue mid-sweep re-flags us.
     */
    timer_ops_delayed = 0;

    /* Detach the retired-queue list under the wheel lock, then free outside. */
    WHEEL_LOCK();
    queue = queue_dead;
    queue_dead = NULL;
    WHEEL_UNLOCK();

    while (queue) {
        next = queue->next;
        kill_queue(queue);
        queue = next;
    }
}


/* Initialize the timer subsystem at boot.
 *
 * Creates the monotonic and TOD timer queues and links them into the queue
 * list.
 */
void rdecl timer_init(void)
{
    kspinlock_init(&timer_wheel_slock, LOCK_CLASS_TIMER, "timer_wheel_slock");
    tod_timers = new_queue(4, 0);
    tod_timers->flags = TQ_FLAG_TOD;
    mon_timers = new_queue(4, 0);
    mon_timers->next = tod_timers;
    queue_head = mon_timers;
}


/* Adopt a new slot resolution for a timer queue chain.
 *
 * Updates the downshift in place when the queue is idle, otherwise chains in a
 * fresh queue with the new resolution. Parameters:
 *   head      : address of the queue-chain head pointer
 *   downshift : new right-shift for slot indexing
 */
static void new_period(struct timer_queue **head, unsigned downshift)
{
    struct timer_queue *new;
    struct timer_queue *old;

    old = *head;
    if (old->downshift != downshift) {
        if (old->num_active == 0) {
            /* nobody using it, just update entry */
            old->downshift = downshift;
        } else {
            new = new_queue(old->mask + 1, downshift);
            if (new) {
                ADD_IN_NEW_QUEUE(head, old, new);
            }
        }
    }
}


/* Recompute timer-queue slot resolution after a clock-resolution change.
 *
 * Picks a downshift so each slot spans roughly one tick, then applies it to
 * both the monotonic and TOD queues.
 */
void rdecl timer_period(void)
{
    unsigned long incr;
    unsigned downshift;

    /* Clock resolution has been updated, need to change the downshift value in
     * the timer queues. We want to choose a value such that the difference
     * between the start and end time that goes into each slot is approximately
     * one tick's worth. That way the timer_expiry() code will usually need to
     * examine only 1 or 2 slots worth of data.
     */

    downshift = 0;
    incr = SYSPAGE_ENTRY(qtime)->nsec_inc;
    do {
        ++downshift;
        incr >>= 1;
    } while (incr != 0);

    WHEEL_LOCK();
    new_period(&mon_timers, downshift);
    new_period(&tod_timers, downshift);
    WHEEL_UNLOCK();
}


/* Report the time left before a timer fires. Parameters:
 *   tip  : timer to query
 *   left : receives the remaining and interval times (zeroed if inactive)
 */
void rdecl timer_remaining(ktimer_t *tip, struct _itimer *left)
{
    uint64_t curtime;

    if (!(tip->flags & RTMUX_TI_ACTIVE)) {
        left->nsec = left->interval_nsec = 0;
        return;
    }

    left->interval_nsec = tip->itime.interval_nsec;

    snap_time(&curtime, tip->flags & RTMUX_TI_TOD_BASED);
    left->nsec = tip->itime.nsec - curtime;

    /* Don't return negative time. Limit it to zero. */
    if ((int64_t) left->nsec < 0) {
        left->nsec = 0;
    }
}


/* Test whether a timer's fire time has already passed.
 *
 * A relative wait of 0 is considered past. Parameters:
 *   clockid  : clock id (selects monotonic vs TOD for absolute timers)
 *   flags    : timer flags (TIMER_ABSTIME for an absolute deadline)
 *   firetime : the fire time to test
 * Return value: 1 if the time has passed, 0 otherwise.
 */
int rdecl timer_past(int clockid, int flags, uint64_t *firetime)
{
    uint64_t curtime;

    if (flags & TIMER_ABSTIME) {
        snap_time(&curtime, (clockid == CLOCK_MONOTONIC) ? 0 : 1);
        if (*firetime < curtime) {
            return 1;
        }
    } else if (*firetime == 0) {
        return 1;
    }

    return 0;
}


/* Activate a timer, inserting it into the appropriate queue.
 *
 * A past-deadline absolute timer fires immediately (inline sigevent delivery in
 * kernel extension context) and is re-armed; otherwise the timer is inserted at
 * its fire time. Parameters:
 *   tip : timer to activate
 */
static void timer_activate_impl(ktimer_t *tip, int defer)
{
    uint64_t tod;
    struct sigevent fire_event;
    const struct sigevent *fire_evp = NULL;
    thread_t *fire_thp = NULL;
    uint8_t fire_reserved = 0;

    WHEEL_LOCK();

    CRASHCHECK(tip->queue);
    tip->flags |= RTMUX_TI_ACTIVE;

    if (tip->flags & RTMUX_TI_ABSOLUTE) {
        SNAP_TIME_INLINE(tod, tip->flags & RTMUX_TI_TOD_BASED);
        if (tip->itime.nsec <= tod) {
            /* Past-deadline absolute timer. Do the wheel re-arm under the lock,
             * then capture the (event, thread) to deliver AFTER releasing it.
             * Delivery must not happen under timer_wheel_slock: it re-enters
             * via timeout_stop → timer_deactivate, which takes the same
             * non-recursive lock (same-hart self-deadlock). This matches the
             * deferred-delivery model timer_expiry already uses.
             */
            fire_event = tip->event;
            fire_evp = &fire_event;
            fire_thp = tip->thread;
            fire_reserved = tip->nresv;
            timer_rearm(tip, FS_ACTIVATE);
        } else {
            timer_insert(tip, FS_ACTIVATE);
        }
    } else {
        timer_insert(tip, FS_ACTIVATE);
    }

    WHEEL_UNLOCK();

    if (fire_evp) {
        if (defer) {
            /* Caller (block_release via timeout_start_deferred) still holds the
             * turnstile chain lock that force_ready would re-acquire. Stash the
             * fire on this CPU; the caller drains it with
             * timeout_deferred_fire() once that lock is released.
             */
            unsigned cpu = get_cpunum();
            CRASHCHECK(timeout_fire_pending[cpu].valid);
            timeout_fire_pending[cpu].event = fire_event;
            timeout_fire_pending[cpu].thp = fire_thp;
            timeout_fire_pending[cpu].reserved = fire_reserved;
            timeout_fire_pending[cpu].valid = 1;
        } else {
            /* Kext context has INKERNEL_NOW set, so notification_deliver's
             * inline xfer path is available and correct (the helper passes
             * straight through to sigevent_exe).
             */
            sigevent_deliver_inline(fire_evp, fire_thp, fire_reserved);
        }
    }
}

void rdecl timer_activate(ktimer_t *tip)
{
    timer_activate_impl(tip, 0);
}

/* Deliver this CPU's deferred timeout fire, if any. Called by block_release()
 * after it has released the turnstile chain lock. No-op when nothing is
 * pending.
 */
void rdecl timeout_deferred_fire(void)
{
    unsigned cpu = get_cpunum();

    if (timeout_fire_pending[cpu].valid) {
        struct sigevent ev = timeout_fire_pending[cpu].event;
        thread_t *thp = timeout_fire_pending[cpu].thp;
        uint8_t reserved = timeout_fire_pending[cpu].reserved;

        timeout_fire_pending[cpu].valid = 0;

        /* The thread's wait may have been completed by another CPU between the
         * WAIT_PENDING clear and this drain (a real wake runs timeout_stop,
         * which clears TIMEOUT_ACTIVE). Delivering then would time out a later
         * wait of the same thread, so drop the stale fire.
         */
        if (!(thp->timeout_flags & RTMUX_FLG_TIMEOUT_ACTIVE))
            return;
        sigevent_deliver_inline(&ev, thp, reserved);
    }
}


/* Deactivate a timer, removing it from its queue.
 *
 * Runs entirely under timer_wheel_slock, so no concurrent timer_expiry/
 * timer_fire can be mutating this timer: the timer is either still linked on a
 * live slot list (unlink it) or already off it. Parameters:
 *   tip : timer to deactivate
 * Return value: non-zero if the timer was still active, 0 otherwise.
 */
int rdecl timer_deactivate(ktimer_t *tip)
{
    unsigned flags;
    struct timer_queue *queue;

    WHEEL_LOCK();
    flags = tip->flags;
    /* Set interval to zero to prevent timer_rearm() from doing anything */
    tip->itime.interval_nsec = 0;
    queue = tip->queue;
    if (queue) {
        /* The wheel lock serializes us against timer_expiry/timer_fire, so the
         * old pending-list rendezvous is gone — just clear the queue pointer
         * for any later reader and unlink below.
         */
        tip->queue = NULL;
    }

    if (tip->flags & RTMUX_TI_ACTIVE) {
        int was_next =
            timer_deadline_mono(tip) ==
            atomic_load_explicit(&cached_next_deadline, memory_order_relaxed);
        tip->flags &= (uint8_t) ~RTMUX_TI_ACTIVE;

        /* Unlink under the wheel lock; no concurrent timer_expiry can touch
         * this slot list.
         */
        CHECK_LINKAGES(tip);
        tip->link.next->link.prev = tip->link.prev;
        tip->link.prev->link.next = tip->link.next;
        CRASHCHECK(!queue);
        --queue->num_active;
        if (was_next)
            timer_recompute_deadline();
    }

    WHEEL_UNLOCK();
    return flags & RTMUX_TI_ACTIVE;
}


/* Allocate a timer object for a process. Parameters:
 *   prp : owning process
 * Return value: the new ktimer_t (with its per-object lock initialized), or
 *               NULL on allocation failure.
 */
ktimer_t *rdecl timer_alloc(process_t *prp)
{
    ktimer_t *tip = object_alloc(prp, &timer_souls);
    if (tip) {
        /* Per-object lock for the per-tip state fields
         * (flags/itime/event/thread/overruns/clockid). Initialized here because
         * object_alloc returns recycled storage whose spinlock state from a
         * previous owner is meaningless. See
         * doc/tex/KerFuncRef/ch06-timers-clocks.tex for the migration plan.
         */
        kspinlock_init(&tip->lock, LOCK_CLASS_TIMER, "ktimer_t.lock");

        /* Set explicitly rather than relying on allocator clearing: nresv gates
         * an object_unreserve on every destroy/create-failure path, so a stale
         * bit from a recycled soul would double-release a reservation.
         */
        tip->nresv = 0;
    }
    return tip;
}


/* Free a deactivated timer object.
 *
 * Synchronizes against any in-flight timer_expiry so it cannot reference the
 * timer being freed. Parameters:
 *   prp : owning process
 *   tip : timer to free (must be deactivated and not pending)
 */
void rdecl timer_free(process_t *prp, ktimer_t *tip)
{
    /* Take and release the wheel lock: any timer_expiry on another hart that
     * was mid-walk has now finished (it holds the lock across the whole walk),
     * and a fresh one cannot reference this timer because it has been
     * deactivated and is no longer on any active list.
     *
     * The post-unlock deferred-sigevent drain in timer_expiry copies each
     * sigevent BY VALUE before releasing the lock, so it holds no pointer into
     * tip; freeing tip here cannot be observed as a use-after-free by a drain
     * that races between the WHEEL_UNLOCK() and the delivery loop.
     */
    WHEEL_LOCK();
    WHEEL_UNLOCK();

    CRASHCHECK(tip->queue);
    CRASHCHECK(tip->pending);

    /* Release the notification-soul reservation this timer took at create, but
     * only after any by-value deferred fire of this timer has drained. Draining
     * first keeps the reserve floor covering the last fire's reserved pop, so
     * lowering it here cannot let non-RT allocation steal a live source's
     * reserved slot (see timer_fire_deferred_flush).
     */
    if (tip->nresv) {
        timer_fire_deferred_flush();
        object_unreserve(&notification_souls);
    }
    object_free(prp, &timer_souls, tip);
}


/* Fault/preempt handler for timer_next — releases the wheel-lock guard.
 * Parameters:
 *   thp  : faulting thread (unused)
 *   regs : register context (unused)
 */
static void timer_next_preempt(thread_t *thp, CPU_REGISTERS *regs)
{
    WHEEL_UNLOCK();
}

static const struct fault_handlers timer_next_handlers = {
    .fault = 0,
    .restart = timer_next_preempt,
};


/* Find the fire time of the earliest non-soft active timer.
 *
 * Scans all timer queues (TOD-adjusting monotonic timers) for the smallest
 * upcoming fire time. Parameters:
 *   np : receives the earliest fire time, or ~0 if none
 */
void rdecl timer_next(uint64_t *np)
{
    struct timer_queue *queue;
    ktimer_t *tip;
    ktimer_t *first;
    unsigned i;
    uint64_t tspec;
    uint64_t check;
    uint64_t tod_adj;

    tspec = ~(uint64_t) 0;

    SET_XFER_HANDLER(&timer_next_handlers);
    WHEEL_LOCK();

    {
        extern volatile uint64_t *nssptr;
        do {
            check = qtimeptr->nsec;
            tod_adj = qtimeptr->nsec_tod_adjust;

            /* make sure we don't pick up the tod adjust value while it's being
             * updated
             */
        } while (check != *nssptr);
    }

    for (queue = queue_head; queue; queue = queue->next) {
        if (queue->num_active != 0) {
            for (i = 0; i <= queue->mask; ++i) {
                tip = first = (ktimer_t *) &queue->slot[i];
                for (;;) {
                    tip = tip->link.next;
                    if (tip == first)
                        goto next;
                    if ((tip->clockid != CLOCK_SOFTTIME) && !tip->pending)
                        break;
                }
                check = tip->itime.nsec;
                if (!(tip->flags & RTMUX_TI_TOD_BASED)) {
                    check += tod_adj;
                }
                if (check < tspec)
                    tspec = check;
            next:;
            }
        }
    }
    WHEEL_UNLOCK();
    SET_XFER_HANDLER(NULL);

    *np = tspec;
}


/* Arm a thread's timeout timer for its current blocking state.
 *
 * Skips non-kernel-call states (waitpage/stack/waitctx) and guards against
 * double activation. Parameters:
 *   thp : thread whose timeout to start
 */
static void timeout_start_impl(thread_t *thp, int defer)
{
    if (thp->timeout && (thp->timeout_flags & (1 << thp->state))) {
        switch (thp->state) {
        case STATE_WAITPAGE:
        case STATE_STACK:
        case STATE_WAITCTX:
            /* These are non-kernel call triggered states, so they shouldn't
             * cause a __timer_timeout() to activate.
             */
            break;
        default:
            /* The timeout_start() function can be called multiple times for the
             * same timer (e.g block_and_ready(), which then invokes the block()
             * function will both call it). So we have to be careful not to try
             * to call timer_activate() multiple times.
             */
            if (!(thp->timeout_flags & RTMUX_FLG_TIMEOUT_ACTIVE)) {
                thp->timeout_flags |= RTMUX_FLG_TIMEOUT_ACTIVE;

                /* A thread mid-transition into an IPC/join wait has
                 * WAIT_PENDING set; force_ready spins on that bit, so a
                 * past-deadline fire delivered inline here would spin on the
                 * thread's own bit forever (the timed wait never returns).
                 * Defer it: the transition's owner drains it with
                 * timeout_deferred_fire() once the bit is clear.
                 */
                if (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
                    defer = 1;
                timer_activate_impl(thp->timeout, defer);
            }
            break;
        }
    }
}

void rdecl timeout_start(thread_t *thp)
{
    timeout_start_impl(thp, 0);
}

/* Like timeout_start(), but a past-deadline timeout's sigevent is captured on
 * this CPU instead of delivered inline. For callers that arm the timeout while
 * holding the turnstile chain lock (block_release): they must call
 * timeout_deferred_fire() after releasing that lock to deliver it.
 */
void rdecl timeout_start_deferred(thread_t *thp)
{
    timeout_start_impl(thp, 1);
}


/* Cancel a thread's active timeout timer.
 *
 * For a nanosleep timeout, records the time remaining before deactivating;
 * skips non-kernel-call states. Parameters:
 *   thp : thread whose timeout to stop
 */
void rdecl timeout_stop(thread_t *thp)
{
    ktimer_t *tip;

    switch (thp->state) {
    case STATE_WAITPAGE:
    case STATE_STACK:
    case STATE_WAITCTX:
        /* These are non-kernel call triggered states, so they shouldn't
         * deactivate a __timer_timeout()
         */
        break;
    default:
        if (thp->timeout_flags & RTMUX_FLG_TIMEOUT_ACTIVE) {
            tip = thp->timeout;
            if (!tip)
                crash();

            /* This is a special case since it returns a value. */
            if (thp->state == STATE_NANOSLEEP) {
                /* Calculate time remaining if we were interrupted. */
                atomic_or(&thp->flags, RTMUX_FLG_THR_NANOSLEEP);
                /* Save time unslept for. */
                if (KSTATUS(thp) != 0) {
                    timer_remaining(thp->timeout, &thp->args.to.left);
                } else {
                    thp->args.to.left.nsec = 0;
                }
            }
            timer_deactivate(tip);
        }
        thp->timeout_flags = 0;
        break;
    }
}
