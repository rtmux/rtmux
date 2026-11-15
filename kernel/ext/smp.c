/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * SMP bring-up — kernel-side body invoked by roottask late in message_start()
 * to push the rest of the harts out of WFI and enable normal cross-CPU thread
 * dispatch. Encapsulates the per-hart state (alives[] / hart_ids[] /
 * cpu_start_others / _smpstart / smp_dispatch_enabled / send_ipi) that U-mode
 * roottask cannot read, so it lives here behind a single
 * syscall_priv(__KSYS_PRIV_SMP_BRINGUP, NULL) call.
 */

#include <core/globals.h>
#include <sbi.h>

extern unsigned hart_ids[];

/* How long to wait for each secondary hart to reach _smpstart, and then its
 * idle loop. Assumption class: tuning, measured with rdtime against the FDT
 * timebase-frequency. A hart takes microseconds under QEMU and on the FU740;
 * one that has not arrived after half a second is treated as dead. Revisit
 * trigger: a platform whose SBI hart_start is slower.
 */
#define SMP_START_WAIT_MS 500

/* Built whatever CONFIG_PROCESSORS_MAX is: cpu_start_others() in
 * arch/riscv/cpu-misc.c calls smp_inject() in every CONFIG_DEBUG_INJECT build.
 */
#ifdef CONFIG_DEBUG_INJECT
/* Debug-only boot options that make the bring-up failure paths reachable under
 * QEMU: "rtmux.inject=hart-start-fail" fails the SBI start of the last CPU, and
 * "rtmux.inject=hart-start-late" starts it only after the wait for it has
 * expired, so it must park.
 */
static bool smp_cmdline_has(const char *opt)
{
    extern char boot_command_line_orig[];
    const char *p = boot_command_line_orig;
    size_t n = strlen(opt);

    while (*p) {
        while (*p == ' ')
            p++;
        if (!strncmp(p, opt, n) && (p[n] == ' ' || p[n] == '\0'))
            return true;
        while (*p && *p != ' ')
            p++;
    }
    return false;
}

/* smp_inject — true if the boot option opt is set and cpu is the last CPU, the
 * one the injected bring-up faults hit.
 */
bool smp_inject(unsigned cpu, const char *opt)
{
    return cpu == num_processors - 1 && smp_cmdline_has(opt);
}
#endif

#if CONFIG_PROCESSORS_MAX > 1
/* smp_wait_join — wait until cpu commits to joining (JOINED), at most ticks of
 * rdtime from the moment its start was asked. On expiry, abandon its slot
 * whether the hart is still on its way (OPEN) or between _smpstart and cpu_idle
 * (ARRIVED); a hart that loses that race parks.
 *
 * Return value: true if the CPU joined.
 */
static bool smp_wait_join(unsigned cpu, uint64_t ticks)
{
    for (;;) {
        uint32_t state = __atomic_load_n(&smp_slot[cpu], __ATOMIC_ACQUIRE);

        if (state == SMP_SLOT_JOINED)
            return true;
        if (state == SMP_SLOT_ABANDONED)
            return false; /* its SBI start failed */
        if (cpu_clock_ticks() - smp_start_tick[cpu] >= ticks &&
            __atomic_compare_exchange_n(&smp_slot[cpu], &state,
                                        SMP_SLOT_ABANDONED, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return false;
        MEM_BARRIER_RW();
    }
}

#ifdef CONFIG_DEBUG_INJECT
/* smp_inject_late_restart — start the hart of a CPU already given up on, and
 * report whether it reached _smpstart (and parked there) within ticks.
 */
static void smp_inject_late_restart(unsigned cpu, uint64_t ticks)
{
    extern void _smpstart(void);
    uintptr_t start = (uintptr_t) _smpstart;
    uint64_t t0;
    bool arrived;

    if (CPU_VADDR_IN_RANGE(start))
        start = CPU_V2P(start);
    (void) sbi_hart_start(hart_ids[cpu], start, cpu);
    t0 = cpu_clock_ticks();
    while (!(arrived = __atomic_load_n(&smp_slot[cpu], __ATOMIC_ACQUIRE) ==
                       SMP_SLOT_ARRIVED) &&
           cpu_clock_ticks() - t0 < ticks)
        MEM_BARRIER_RW(); /* _smpstart swaps ARRIVED in, then parks */
    kprintf("smp: hart %u (cpu %u) %s\n", hart_ids[cpu], cpu,
            arrived ? "arrived late, parked" : "still absent");
}
#endif

/* smp_drop_cpu — take a CPU whose hart did not start out of LEGAL_CPU_BITMASK,
 * so the scheduler never places a thread on it. Its pc_alive stays false, which
 * every cross-CPU wait (RCU, TLB shootdown, hartwatch) already skips.
 */
static void smp_drop_cpu(unsigned cpu)
{
    kprintf("smp: hart %u (cpu %u) did not start, dropped\n", hart_ids[cpu],
            cpu);
    __atomic_and_fetch(&legal_cpu_bitmask, ~(1ULL << cpu), __ATOMIC_RELEASE);
}
#endif

/* kext_smp_bringup — start the secondary harts and open the SMP dispatch gate.
 *
 * Starts each AP at _smpstart, gives it SMP_START_WAIT_MS from its start
 * request to commit to joining, pokes it with IPI_RESCHED, then enables
 * cross-CPU scheduling. A hart that does not join in time is dropped from
 * LEGAL_CPU_BITMASK and parks if it turns up later. Parameters:
 *   unused : ignored
 */
void kext_smp_bringup(void *unused)
{
    (void) unused;

#if CONFIG_PROCESSORS_MAX > 1
    extern void _smpstart(void);
    uint64_t ticks = qtimeptr->cycles_per_sec / 1000 * SMP_START_WAIT_MS;
    unsigned me = get_cpunum();
    unsigned online = 1; /* this hart */

    /* Start every secondary hart at _smpstart (SBI HSM hart_start). Pool
     * threads were enqueued before this call, so the IPIs from ready_default()
     * were lost on harts that hadn't booted yet — we have to re-deliver
     * IPI_RESCHED below once each AP is alive.
     */
    online += cpu_start_others((uintptr_t) _smpstart);

    /* Busy-wait, boot-time only. Every hart was started above, so its deadline
     * runs from its own start request and the waits overlap.
     */
    for (unsigned cpu = 0; cpu < num_processors; cpu++) {
        uint64_t t0;

        if (cpu == me)
            continue;
        if (!smp_wait_join(cpu, ticks)) {
            smp_drop_cpu(cpu);
#ifdef CONFIG_DEBUG_INJECT
            if (smp_inject(cpu, "rtmux.inject=hart-start-late"))
                smp_inject_late_restart(cpu, ticks);
#endif
            continue;
        }

        /* It joined, so pc_alive follows within a few instructions; not getting
         * there is a kernel bug.
         */
        t0 = cpu_clock_ticks();
        while (!__atomic_load_n(&pcpu_array[cpu].pc_alive, __ATOMIC_ACQUIRE)) {
            if (cpu_clock_ticks() - t0 >= ticks)
                crash("smp: hart %u (cpu %u) arrived but never came alive",
                      hart_ids[cpu], cpu);
            MEM_BARRIER_RW();
        }
        SENDIPI(cpu, IPI_RESCHED);
        online++;
    }
    kprintf("smp: %u harts online\n", online);
#endif

    /* Open the per-CPU dispatch gate. Until this point, idle.c and dispatch.c
     * kept secondary harts off the ready queue so they couldn't pick threads
     * while roottask was still wiring up its pool. From here on the scheduler
     * is fully SMP.
     */
    smp_dispatch_enabled = 1;
}
