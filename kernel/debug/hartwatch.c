/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * hartwatch.c — per-hart neighbour watchdog (CONFIG_STALL_DETECTOR).
 *
 * The heartbeat stall detector (stall-detector.c) keys on the boot hart's clock
 * and goes blind if that hart wedges. Here every hart instead watches the next
 * online entry of hart_ids[] (the monitor hart is not in that list): each
 * supervisor timer interrupt a hart takes bumps its pc_timer_progress, and the
 * watcher names a neighbour whose counter has not moved for
 * HARTWATCH_TIMEOUT_SEC of rdtime. An idle hart still wakes at least every
 * IDLE_MAX_SLEEP_NS (50 ms), so no idle exemption exists: a silent hart is
 * either spinning with interrupts off or dead.
 *
 * Shared-state invariant:
 *   - pc_timer_progress: single writer (the owner hart, relaxed fetch_add in
 *     the timer trap), any reader (relaxed load; "did it move" needs no order).
 *   - hw_seen/hw_stamp/hw_latched[t]: written by the watcher of hart t, which
 *     is one hart at a time. If the topology changes and two watchers briefly
 *     overlap, the latch is claimed with an atomic exchange, so at most one of
 *     them reports; the other fields are idempotent.
 *   - Nothing here takes a lock or waits on another hart. The report goes out
 *     through kprintf_emergency(): the console lock gets a bounded number of
 *     tries, after which the line is written to the console without it (the
 *     stalled hart may be the lock's owner for good), so the report is never
 *     lost or delayed and no hart's hold on the lock is ever cleared.
 *
 * Each hart polls at most every HARTWATCH_POLL_MS (per-hart deadline, touched
 * only by that hart).
 *
 * One report is emitted per silent episode; the latch re-arms when the hart's
 * counter moves again.
 *
 * License: Apache-2.0
 */

#include <core/globals.h>
#include <stdatomic.h>

#ifdef CONFIG_STALL_DETECTOR

/* Silence that names a hart, in seconds of rdtime. */
#define HARTWATCH_TIMEOUT_SEC 2U

/* Each hart re-checks its neighbour at most this often (milliseconds of
 * rdtime). A busy hart takes a timer interrupt every tick (1 kHz at the default
 * CONFIG_TIMER_HZ), and the watch needs nothing like that resolution: the rate
 * limit keeps the common case to one rdtime read and a compare, which is what
 * holds the preempt-latency cost of the option down.
 */
#define HARTWATCH_POLL_MS 50U

/* Console-lock acquisition attempts before the report is written without the
 * lock (and before a parked line is left for later).
 */
#define HARTWATCH_LOCK_RETRIES 20000U

/* Per-watcher poll deadline, one cache line per hart; each entry is touched
 * only by its own hart, so there is no sharing.
 */
static struct {
    uint64_t next;
} __attribute__((aligned(64))) hw_poll[CONFIG_PROCESSORS_MAX];

/* Indexed by the watched hart ID. */
static _Atomic unsigned long hw_seen[CONFIG_PROCESSORS_MAX]; /* progress seen */
static _Atomic uint64_t hw_stamp[CONFIG_PROCESSORS_MAX]; /* rdtime of that */
static atomic_bool hw_latched[CONFIG_PROCESSORS_MAX];    /* reported */

/* hartwatch_report — name a silent hart. Parameters:
 *   target : hart ID that stopped taking timer interrupts.
 *   stamp  : rdtime at which its progress counter last moved.
 *   now    : current rdtime.
 * Runs in timer-trap context. The target's active thread is read inside an
 * RCU-sched read section: this hart is in interrupt context and so cannot pass
 * a quiescent state, which keeps the thread and its process from being freed
 * under us.
 */
static void hartwatch_report(unsigned target, uint64_t stamp, uint64_t now)
{
    uint64_t cps = qtimeptr->cycles_per_sec;
    int tid = -1;
    int pid = -1;
    thread_t *thp;

    rcu_read_lock(&tthread_rcu);
    rcu_read_lock(&process_rcu);
    thp = get_active_thread(target);
    if (thp) {
        tid = (int) thp->tid;
        if (thp->process)
            pid = thp->process->pid;
    }
    rcu_read_unlock(&process_rcu);
    rcu_read_unlock(&tthread_rcu);

    kprintf_emergency(
        HARTWATCH_LOCK_RETRIES,
        "hartwatch: hart %u silent for %lu ms, last timer progress "
        "at t=%lu ms, active pid=%d tid=%d\n",
        target, (unsigned long) ((now - stamp) * 1000 / cps),
        (unsigned long) (stamp * 1000 / cps), pid, tid);
}

/* hartwatch_tick — check this hart's neighbour. Called from every hart's
 * supervisor-timer trap (arch/riscv/trap-handler.c), SIE off, before the
 * in-kernel short-circuit. Lock-free; the report is the only slow step and runs
 * once per silent episode.
 */
void hartwatch_tick(void)
{
    extern unsigned hart_ids[];
    unsigned n = num_processors;
    unsigned self = get_cpunum();
    unsigned si = 0;
    unsigned target = ~0U;
    unsigned long cur;
    uint64_t now, stamp;

    now = r_time();
    if (now < hw_poll[self].next)
        return;
    hw_poll[self].next =
        now + qtimeptr->cycles_per_sec / 1000 * HARTWATCH_POLL_MS;

    /* A report parked behind a busy console lock goes out as soon as the lock
     * is free, not only on this hart's next unrelated print.
     */
    kprintf_flush_parked(HARTWATCH_LOCK_RETRIES);
    if (n < 2 || !pcpu_array[self].pc_alive)
        return;
    while (si < n && hart_ids[si] != self)
        si++;
    if (si == n)
        return;
    for (unsigned k = 1; k < n; k++) {
        unsigned h = hart_ids[(si + k) % n];
        if (__atomic_load_n(&pcpu_array[h].pc_alive, __ATOMIC_ACQUIRE)) {
            target = h;
            break;
        }
    }
    if (target == ~0U)
        return;

    cur = atomic_load_explicit(&pcpu_array[target].pc_timer_progress,
                               memory_order_relaxed);
    stamp = atomic_load_explicit(&hw_stamp[target], memory_order_relaxed);
    if (!stamp ||
        cur != atomic_load_explicit(&hw_seen[target], memory_order_relaxed)) {
        /* First look, or the hart moved: restart its window and re-arm. */
        atomic_store_explicit(&hw_seen[target], cur, memory_order_relaxed);
        atomic_store_explicit(&hw_stamp[target], now ? now : 1,
                              memory_order_relaxed);
        atomic_store_explicit(&hw_latched[target], false, memory_order_relaxed);
        return;
    }

    if (now - stamp <
        (uint64_t) HARTWATCH_TIMEOUT_SEC * qtimeptr->cycles_per_sec)
        return;
    if (atomic_exchange_explicit(&hw_latched[target], true,
                                 memory_order_relaxed))
        return;
    hartwatch_report(target, stamp, now);
}

#endif /* CONFIG_STALL_DETECTOR */
