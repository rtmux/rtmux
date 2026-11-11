/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* force recompile */
#define KERNEL_DEFN

#include <core/globals.h>

/* The per-hart "currently active thread" pointer lives in
 * pcpu_array[hart].pc_curthread (offset 0 of struct pcpu).
 * get_active_thread/set_active_thread are the accessor pair; cross-hart
 * self-access goes through get_curthread() (one instruction: "ld reg, 0(tp)").
 */

/* Per-CPU "currently inside a kernel call" depth counter.
 *
 * Set to 1 by __kcall_dispatch right before release_kernel() for a BKL-lifted
 * handler, and cleared after the post-handler acquire_kernel(). Used by
 * in_kernel_context() so that the timer trap handler keeps taking the minimal
 * "rearm only" path during the BKL-released window of a lifted kcall — running
 * the full clock-handler chain there leaks BSS-global addresses through C
 * caller-saved registers into the active thread's reg save area.
 *
 * Always-zero outside the BKL-released kcall window, so it costs nothing for
 * ordinary (BKL-held) kernel work.
 */
DEFINE_PER_CPU(unsigned, in_kcall);

/* Per-CPU "currently inside specialret()" flag, in
 * pcpu_array[hart].pc_in_specret.
 *
 * Two CPUs can each have their own thread in specialret simultaneously, so the
 * flag is per-CPU to give each fault handler / xfer-fault path the correct view
 * of "is THIS CPU in specialret right now?". Set to 1 on entry to specialret,
 * back to 0 on exit. Not a nesting counter: the long-standing assertion that
 * specialret is non-reentrant (special-return.c) is preserved.
 */

/* Per-CPU object-allocator override, in
 * pcpu_array[hart].pc_soul_alloc_override. See lock.h for semantics. Default 0
 * (= SOUL_OVERRIDE_NONE) so existing callers see the soul's flags unchanged.
 */

/* Per-CPU "pending preemption priority", in
 * pcpu_array[hart].pc_pending_preempt_prio. Written by
 * sigevent_deliver_inline() in trap context when an IRQ-side wake makes a
 * thread runnable that is higher priority than the kernel extension currently
 * mid-execution on this hart. Read by NEED_PREEMPT (macros.h) from long-running
 * kernel extension loops (xfer, ext/misc, ext/debug, timer batch) to decide
 * whether to KCALL_RESTART and yield to dispatch.
 *
 * Reset to 0 in kernel_exit once dispatch has selected the next active thread —
 * the "pending preemption" is satisfied by the dispatch.
 *
 * Single-writer-per-cpu (the trap on hart X writes prio[X]; no cross-CPU
 * writes), so no atomic needed — plain uint8 store.
 */

/* Per-CPU "last interrupted PC", in pcpu_array[hart].pc_last_kernel_pc.
 * strap_handler_c updates this on every S-mode trap entry, using the trap-frame
 * PC (sepc). When a hart is spinning inside the kernel and a timer interrupt
 * fires, the saved PC IS the kernel-mode spinning location — exactly what the
 * monitor needs to identify which kernel extension is stuck. For U-mode traps
 * the value is the U-mode entry PC (less useful but harmless).
 */

/* Per-CPU "last scause" + IPI receive counter, in
 * pcpu_array[hart].pc_{last_scause,ipi_handler_count}. Updated at trap entry.
 * Lets the monitor confirm whether a remote hart is taking IPI traps (cause=1 =
 * supervisor software int) — and how many times it has run ipi_handler.
 */

/* Per-CPU aggregated control structure.
 *
 * One slot per hart, indexed by hart ID. Allocated as BSS — zero-init is the
 * correct initial state for every field (pc_curthread starts as the idle
 * thread, pc_hart is stamped by bootstrap).
 *
 * Defined here so it lands adjacent to the other per-CPU storage and stays
 * inside the .data/.bss layout that monitor.c and other tools walk. Declared in
 * <arch/pcpu.h>.
 */
struct pcpu pcpu_array[PROCESSORS_MAX];

/* in_kcall accessors. Wrapping the per-CPU counter behind a function boundary
 * serves two goals:
 *
 *   1. Self-cpu invariant: in_kcall is incremented/decremented only
 *      by the owning hart.  The accessors take no "cpu" argument; they
 *      call get_cpunum() internally.  Cross-CPU writes are impossible
 *      at the source.
 *
 *   2. RCU-sched ordering: inc publishes a non-zero depth before later
 *      kcall reads; the final dec publishes zero only after earlier
 *      kcall reads are complete. rcu_sched_wait() acquire-loads this
 *      counter when treating zero as a quiescent state.
 *
 *   3. Underflow detection: a stray dec on a slot that was already 0
 *      crashes immediately with caller PC, instead of producing a
 *      cascading CRASHCHECK far away in some kernel extension.
 *
 * The raw "in_kcall[]" array remains exported for cross-hart observers such as
 * RCU-sched and the monitor thread. Self-hart code MUST go through the
 * accessors.
 */
void in_kcall_inc(void)
{
    unsigned cpu = get_cpunum();
    unsigned cur = __atomic_load_n(&in_kcall[cpu], __ATOMIC_RELAXED);

    if (cur == 0)
        __atomic_store_n(&pcpu_array[cpu].pc_syscall, IN_KCALL_PHASE_NONE,
                         __ATOMIC_RELAXED);
    __atomic_store_n(&in_kcall[cpu], cur + 1, __ATOMIC_RELEASE);
    cpu_smp_mb();
}

void in_kcall_phase(int phase)
{
    __atomic_store_n(&pcpu_array[get_cpunum()].pc_syscall, phase,
                     __ATOMIC_RELEASE);
}

void in_kcall_dec(void)
{
    unsigned cpu = get_cpunum();
    unsigned cur = __atomic_load_n(&in_kcall[cpu], __ATOMIC_RELAXED);
    unsigned next;

    if (__builtin_expect(cur == 0, 0)) {
        kprintf("in_kcall_dec UNDERFLOW: cpu=%u caller=%lx\n", cpu,
                (unsigned long) __builtin_return_address(0));
        crash();
    }
    next = cur - 1;
    if (next == 0)
        cpu_smp_mb();
    __atomic_store_n(&in_kcall[cpu], next, __ATOMIC_RELEASE);
}

unsigned in_kcall_get_self(void)
{
    return __atomic_load_n(&in_kcall[get_cpunum()], __ATOMIC_RELAXED);
}

#if defined(CONFIG_INSTRUMENTATION)

/* Kernel interrupt-disabled (non-preemptible) window instrumentation (section
 * 18). A kspinlock disables SIE while held, so the span from the outermost
 * kspinlock_lock (pc_kspin_depth 0 to 1) to the matching outermost unlock (1 to
 * 0) is one interrupt-disabled window. Unlike an in_kcall span, this cannot
 * cross a context switch: a thread never blocks or is preempted while holding a
 * spinlock, so each window is one continuous stretch of execution. These
 * per-CPU lock-free counters record window durations in cycles, tagged with the
 * class of the outermost lock; nonpreempt_report() converts to nanoseconds and
 * prints the max plus a log2 histogram that a p99 is read from. Compiled only
 * under CONFIG_INSTRUMENTATION; production builds carry none of this.
 *
 * Scope of this slice: the window is the lock-HELD duration, opened after the
 * acquire, so the acquisition spin (interrupt_lock disables SIE before
 * spinning) is excluded; under the low contention of the measured workload that
 * spin is near zero, so held time approximates the full IRQ-off window. Raw
 * interrupt_disable() / INTR_LOCK spans that do not go through a kspinlock
 * (clock_slock, intr_slock) are likewise not covered.
 */
#define IRQOFF_BUCKETS 40
static uint64_t irqoff_start[CONFIG_PROCESSORS_MAX];
static int irqoff_class[CONFIG_PROCESSORS_MAX];
static uint64_t irqoff_max_cyc[CONFIG_PROCESSORS_MAX];
static int irqoff_max_class[CONFIG_PROCESSORS_MAX];
static uint64_t irqoff_count[CONFIG_PROCESSORS_MAX];
static uint32_t irqoff_hist[CONFIG_PROCESSORS_MAX][IRQOFF_BUCKETS];

/* Called from kspinlock_lock on the outermost acquire (SIE just went off). */
void irqoff_open(int class)
{
    unsigned cpu = get_cpunum();

    irqoff_class[cpu] = class;
    irqoff_start[cpu] = cpu_clock_ticks();
}

/* Called from kspinlock_unlock on the outermost release (before SIE is
 * re-enabled), closing the window opened by irqoff_open.
 */
void irqoff_close(void)
{
    unsigned cpu = get_cpunum();
    uint64_t start = irqoff_start[cpu];
    uint64_t cyc;
    unsigned b;

    if (!start)
        return;
    cyc = cpu_clock_ticks() - start;

    /* floor(log2(cyc)) by shifting; the freestanding kernel does not link the
     * libgcc __clzdi2 that __builtin_clzll would pull in.
     */
    b = 0;
    {
        uint64_t v = cyc;
        while (v > 1) {
            v >>= 1;
            b++;
        }
    }
    if (b >= IRQOFF_BUCKETS)
        b = IRQOFF_BUCKETS - 1;
    irqoff_hist[cpu][b]++;
    irqoff_count[cpu]++;
    if (cyc > irqoff_max_cyc[cpu]) {
        irqoff_max_cyc[cpu] = cyc;
        irqoff_max_class[cpu] = irqoff_class[cpu];
    }
}

/* Print the per-CPU interrupt-disabled window stats: sample count, worst window
 * (max, in ns, with the lock class that owned it), and a log2-bucketed p99.
 * Called throttled from clock_handler on the boot CPU, outside any lock. Reads
 * other harts' slots cross-hart (plain loads), stats-racy by design.
 */
void nonpreempt_report(void)
{
    uint64_t cps = qtimeptr->cycles_per_sec;
    unsigned cpu;
    static int warmed;

    /* Discard everything accumulated before the first report (roughly the first
     * second of boot, so early-init lock windows do not skew the steady-state
     * budget).
     */
    if (!warmed) {
        warmed = 1;
        for (cpu = 0; cpu < CONFIG_PROCESSORS_MAX; cpu++) {
            unsigned i;

            /* Clear start too: a window opened before this reset and closed
             * after it would otherwise be counted once against a stale start.
             */
            irqoff_start[cpu] = 0;
            irqoff_max_cyc[cpu] = 0;
            irqoff_max_class[cpu] = 0;
            irqoff_count[cpu] = 0;
            for (i = 0; i < IRQOFF_BUCKETS; i++)
                irqoff_hist[cpu][i] = 0;
        }
        return;
    }

    if (!cps)
        cps = 1;
    for (cpu = 0; cpu < num_processors; cpu++) {
        uint64_t count = irqoff_count[cpu];
        uint64_t max_ns, p99_target, acc = 0, p99_cyc = 0, p99_ns;
        unsigned i;

        if (!count)
            continue;
        max_ns = irqoff_max_cyc[cpu] * 1000000000ULL / cps;

        /* Coarse (log2 bucket) 99th percentile: first bucket whose cumulative
         * count reaches 99% reports its lower edge 2^i as the cycle count.
         */
        p99_target = count - count / 100;
        for (i = 0; i < IRQOFF_BUCKETS; i++) {
            acc += irqoff_hist[cpu][i];
            if (acc >= p99_target) {
                p99_cyc = (uint64_t) 1 << i;
                break;
            }
        }
        p99_ns = p99_cyc * 1000000000ULL / cps;
        kprintf(
            "irqoff: cpu=%u count=%llu max_ns=%llu max_class=%d p99_ns=%llu\n",
            cpu, (unsigned long long) count, (unsigned long long) max_ns,
            irqoff_max_class[cpu], (unsigned long long) p99_ns);
    }
}
#endif

/* Return the thread currently active on the given hart. Parameters:
 *   cpu : hart ID to query.
 * Return value: the hart's current thread (may be NULL).
 */
thread_t *get_active_thread(unsigned cpu)
{
    return pcpu_array[cpu].pc_curthread;
}

/* Return the thread currently active on the given hart, crashing if it is NULL.
 * Parameters:
 *   cpu : hart ID to query.
 * Return value: the hart's current thread (never NULL).
 */
thread_t *get_active_thread_chk(unsigned cpu)
{
    thread_t *thp = pcpu_array[cpu].pc_curthread;
    if (__builtin_expect(!thp, 0)) {
        kprintf("FATAL: pc_curthread[%u] is NULL\n", cpu);
        crash();
    }
    return thp;
}

/* Set the thread currently active on the given hart.
 *
 * Crashes if thp is a non-NULL but obviously bogus (sub-kernel-VA) pointer,
 * which catches a corrupted thread pointer at the store site. Parameters:
 *   cpu : hart ID to update.
 *   thp : the thread to record as active (NULL is permitted).
 */
void set_active_thread(unsigned cpu, thread_t *thp)
{
    if (thp && (uintptr_t) thp < KERNEL_VIRT_BASE) {
        kprintf("BAD set_active_thread: cpu=%u thp=%p\n", cpu, (void *) thp);
        crash();
    }
    __atomic_store_n(&pcpu_array[cpu].pc_curthread, thp, __ATOMIC_RELEASE);
}

EXT const uint8_t byte_log2[] = {
    0, 0, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4,
    4, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
    6, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7};
