/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/storage.h>
#include <paging.h>


static char *local_envv[16];

/* Initialize kernel state derived from the syspage.
 *
 * Clamps the CPU count, captures CPU flags and the system-private pointer,
 * converts the kernel cpupage to high VA and wires per-CPU cpupage/pcpu state,
 * determines the boot hart and reorders hart_ids[] so the boot hart is first,
 * optionally reserves the last hart for the monitoring kthread, and records the
 * kcall/qtime/intrinfo pointers.
 */
static void syspage_init()
{
    unsigned i;
    struct cpupage_entry *cpupage;
    struct kdebug_callback *kdcall;

    _syspage_ptr->num_cpu =
        (uint16_t) min_uint((unsigned) PROCESSORS_MAX, _syspage_ptr->num_cpu);
    num_processors = _syspage_ptr->num_cpu;

    __cpu_flags = SYSPAGE_ENTRY(cpuinfo)->flags;

    privateptr = SYSPAGE_ENTRY(system_private);

    if ((kdcall = privateptr->debug_call)) {
        kdextra = kdcall->extra;
    }

    /* kern_cpupageptr was set by startup as a PA (identity-mapped). Convert to
     * high VA so it's dereferenceable in kernel context.
     */
    _cpupage_ptr = cpupage =
        (struct cpupage_entry *) ((uintptr_t) privateptr->kern_cpupageptr +
                                  KERNEL_VIRT_BASE);
    cpupage->tls = &intr_tls;
    {
        extern unsigned hart_ids[];
        extern unsigned board_smp_num_cpu(void);
        extern uint64_t _boot_a0;

        /* Set boot_cpunum HERE, not in kernel_init (which runs later).
         * syspage_init must know the boot hart to build cpupageptr correctly.
         */
        boot_cpunum = (unsigned) _boot_a0;

        /* Rebuild hart_ids[0..num_processors-1] with boot hart at [0]. The
         * FDT-populated list may have any ordering, and with -P N the boot hart
         * may be beyond index N-1.
         */
        {
            unsigned fdt_count = board_smp_num_cpu();
            unsigned fdt_harts[PROCESSORS_MAX];
            unsigned idx = 1;

            for (unsigned k = 0; k < fdt_count && k < PROCESSORS_MAX; k++)
                fdt_harts[k] = hart_ids[k];

            hart_ids[0] = boot_cpunum;
            for (unsigned k = 0; k < fdt_count && idx < num_processors; k++) {
                if (fdt_harts[k] != boot_cpunum)
                    hart_ids[idx++] = fdt_harts[k];
            }
        }

#ifdef CONFIG_MONITORING_KTHREAD

        /* Capture the last hart for the monitoring kthread. Set the sentinel
         * before decrementing num_processors so the rest of the kernel never
         * sees the captured hart. All per-CPU init below iterates
         * [0..num_processors-1) and naturally skips it.
         */
        if (num_processors >= 2) {
            monitor_hart_id = hart_ids[num_processors - 1];
            num_processors--;
        }
#endif

        for (i = 0; i < num_processors; ++i) {
            unsigned cpu = hart_ids[i];
            cpupageptr[cpu] = cpupage;
            cpupage->syspage = privateptr->user_syspageptr;
            cpupage->cpu = cpu;
            cpupage =
                (void *) ((uint8_t *) cpupage + privateptr->cpupage_spacing);

            /* Populate each hart's pc_hart so that get_cpunum() = "lw
             * PC_HART(tp)" returns the correct architectural hart ID once an AP
             * sets its tp = &pcpu_array[hart] in _smpstart. For the boot hart
             * this was already populated in startup_entry, but re-writing it
             * with the same value is harmless and keeps the loop uniform across
             * all harts.
             */
            pcpu_array[cpu].pc_hart = cpu;
        }

        /* The boot hart's tp already points at &pcpu_array[boot_cpunum] (set in
         * startup_entry). No re-stamp needed — pcpu_array[boot_cpunum].pc_hart
         * was populated above; get_cpunum() works correctly.
         */
    }

    /* kcallptr must be a user-accessible address: cpu_force_thread_destroyall
     * sets a U-mode thread's PC here so that sret executes the ecall. Points to
     * the dedicated kcall trampoline page (R+X, separate from syspage).
     */
    kcallptr = (void *) RTMUX_KCALL_UADDR;

    qtimeptr = SYSPAGE_ENTRY(qtime);

    intrinfoptr = SYSPAGE_ENTRY(intrinfo);
    intrinfo_num = _syspage_ptr->intrinfo.entry_size / sizeof(*intrinfoptr);

    cpu_syspage_init();
}

/* kernel_entry — called by startup_entry() after syspage is built and MMU has
 * been switched to high VA. Converts the syspage PA to its high-VA address,
 * then calls kernel_init() to initialize all kernel subsystems.
 */
void kernel_entry(struct syspage_entry *sysp)
{
    /* Startup passes the syspage as a PA (it was built before VA switch).
     * Convert to high VA so SYSPAGE_ENTRY() pointer arithmetic works.
     */
    _syspage_ptr =
        (struct syspage_entry *) ((uintptr_t) sysp + KERNEL_VIRT_BASE);

    syspage_init();

    set_inkernel_init(INKERNEL_NOW | INKERNEL_LOCK);

    local_envv[0] = 0;

    /* allow override of the tick multiplier. */
    if (qtimeptr->rr_interval_mul != 0) {
        rr_interval_mul = qtimeptr->rr_interval_mul;
    }

    kernel_init(_argc, _argv, local_envv);
}

/* Kernel-side process exit — panics if ever reached.
 *
 * Kernel-internal threads run forever or terminate via the destroy-thread kcall
 * path from user space, so this should never be called; if it is, there is no
 * S-mode mechanism to tear down a thread. Parameters:
 *   status : would-be exit status (reported in the panic message).
 */
void __exit(int status)
{
    /* Kernel-internal threads (idle, loaders, IST hooks) are not expected to
     * call __exit: they either run forever or terminate via the destroy-thread
     * kcall path from user space. If something here does fall through, panic —
     * there is no S-mode kernel mechanism to tear down a thread now that the
     * calls.c emulation is gone.
     */
    crash_halt("kernel __exit called (status=%d) — no S-mode teardown path",
               status);
}

/* libc _exit() alias — forwards to __exit() and spins forever. Parameters:
 *   status : exit status, passed through to __exit().
 */
void _exit(int status)
{
    __exit(status);
#ifdef __GNUC__
    for (;;) {
        /* nothing to do */
    }
#endif
}

/* C library exit() alias — forwards to _exit() and spins forever. Parameters:
 *   status : exit status, passed through to _exit().
 */
void exit(int status)
{
    _exit(status);
#ifdef __GNUC__
    for (;;) {
        /* nothing to do */
    }
#endif
}
