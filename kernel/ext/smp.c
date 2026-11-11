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

/* kext_smp_bringup — start the secondary harts and open the SMP dispatch gate.
 *
 * Starts each AP at _smpstart, waits for it to come alive and pokes it with
 * IPI_RESCHED, then enables cross-CPU scheduling. Parameters:
 *   unused : ignored
 */
void kext_smp_bringup(void *unused)
{
    (void) unused;

#if CONFIG_PROCESSORS_MAX > 1
    extern unsigned hart_ids[];
    extern void _smpstart(void);

    /* Start every secondary hart at _smpstart (SBI HSM hart_start). Pool
     * threads were enqueued before this call, so the IPIs from ready_default()
     * were lost on harts that hadn't booted yet — we have to re-deliver
     * IPI_RESCHED below once each AP is alive.
     */
    cpu_start_others((uintptr_t) _smpstart);

    /* Wait for each AP to set pcpu_array[h].pc_alive in its idle loop, then
     * poke it with IPI_RESCHED so it leaves WFI and starts dispatching. Bounded
     * busy-wait: the SBI hart_start is synchronous from this hart's
     * perspective, the remote hart only has to walk to its dispatch loop, and
     * we hold the BKL throughout (kernel work on other harts that needs BKL
     * spins, but IPI handlers don't acquire BKL — no deadlock).
     */
    {
        unsigned me = get_cpunum();
        unsigned online = 1; /* this hart */
        for (unsigned _hi = 0; _hi < num_processors; _hi++) {
            unsigned h = hart_ids[_hi];
            if (h == me)
                continue;
            while (!pcpu_array[h].pc_alive)
                MEM_BARRIER_RW();
            SENDIPI(h, IPI_RESCHED);
            online++;
        }
#ifdef CONFIG_MONITORING_KTHREAD
        if (monitor_hart_id != ~0U)
            online++; /* started by cpu_start_others, not dispatching */
#endif
        kprintf("smp: %u harts online\n", online);
    }
#endif

    /* Open the per-CPU dispatch gate. Until this point, idle.c and dispatch.c
     * kept secondary harts off the ready queue so they couldn't pick threads
     * while roottask was still wiring up its pool. From here on the scheduler
     * is fully SMP.
     */
    smp_dispatch_enabled = 1;
}
