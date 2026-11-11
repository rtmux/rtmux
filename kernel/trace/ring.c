/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* trace-ring.c — Per-CPU function-entry trace ring buffer.
 *
 * Storage + dump implementation. See <trace-ring.h> for the TRACE_FN macro and
 * the function-id enum.
 *
 * License: Apache 2.0
 */

#include <core/globals.h>
#include <stdatomic.h>
#include "trace-ring.h"

#ifdef CONFIG_TRACE_RING

struct trace_entry trace_ring[CONFIG_PROCESSORS_MAX][TRACE_RING_SIZE];
unsigned trace_idx[CONFIG_PROCESSORS_MAX];

static const char *fn_names[FN_NR] = {
    [FN_NONE] = "NONE",
    [FN_KCALL_DISPATCH] = "kcall_dispatch",
    [FN_KCALL_NO_BKL_PRE] = "kcall:nobkl-pre",
    [FN_KCALL_NO_BKL_POST] = "kcall:nobkl-post",
    [FN_KER_MSG_RECEIVEV] = "ksys_msg_receivev",
    [FN_KER_MSG_RECV_BLOCK] = "msg_recv:block",
    [FN_KER_MSG_RECV_RET] = "msg_recv:return",
    [FN_KER_MSG_SENDV] = "ksys_msg_sendv",
    [FN_KER_MSG_REPLYV] = "ksys_msg_replyv",
    [FN_UNREADY] = "unready",
    [FN_BLOCK] = "block",
    [FN_BLOCK_AND_READY] = "block_and_ready",
    [FN_MARK_RUNNING] = "mark_running",
    [FN_READY] = "ready",
    [FN_FORCE_READY] = "force_ready",
    [FN_INTREVENT_DRAIN] = "intrevent_drain (removed)",
    [FN_INTREVENT_ADD] = "intrevent_add (removed)",
    [FN_STRAP_HANDLER_C] = "strap_handler_c",
    [FN_TRAP_TIMER_MIN] = "trap:timer-min",
    [FN_TRAP_TIMER_FULL] = "trap:timer-full",
    [FN_IPI_HANDLER] = "ipi_handler",
    [FN_TRAP_PLIC] = "trap:plic",
    [FN_TRAP_ECALL_U] = "trap:ecall-U",
    [FN_TRAP_ECALL_S] = "trap:ecall-S",
    [FN_KER_EXIT] = "kernel_exit",
    [FN_KER_RESTORE_PRE] = "__kernel_restore-pre",
    [FN_ACQUIRE_KERNEL] = "acquire_kernel",
    [FN_RELEASE_KERNEL] = "release_kernel",
    [FN_KER_PRIV] = "ksys_priv",
    [FN_KER_PRIV_RET] = "ksys_priv:ret",
};

/* Dump the trace ring for the given CPU. Call from a crash path after other
 * CPUs have been parked.
 *
 * Disables interrupts and force-releases kprintf_slock, then prints the entire
 * dump in one stretch. Re-entering the trap handler (nested trap during dump)
 * would still mangle, but at least IRQ- driven kprintfs cannot interrupt this
 * CPU mid-dump.
 */
void trace_ring_dump(unsigned cpu)
{
    if (cpu >= CONFIG_PROCESSORS_MAX) {
        kprintf("trace_ring_dump: invalid cpu %u\n", cpu);
        return;
    }

    /* Disable interrupts on this CPU for the entire dump. */
    unsigned long sstatus_saved;
    __asm__ volatile("csrrci %0, sstatus, %1"
                     : "=r"(sstatus_saved)
                     : "i"(2) /* SSTATUS_SIE bit */);
    (void) sstatus_saved; /* not restored — we're crashing */

    /* Force-release the kprintf lock; we're the only path now. */
    extern atomic_uint kprintf_slock;
    atomic_store_explicit(&kprintf_slock, 0, memory_order_release);

    unsigned end = trace_idx[cpu];
    unsigned start = (end >= TRACE_RING_SIZE) ? end - TRACE_RING_SIZE : 0;

    kprintf("=== trace ring CPU %u (last %u of %u entries) ===\n", cpu,
            end - start, end);

    /* Print oldest-to-newest. */
    for (unsigned i = start; i < end; i++) {
        unsigned slot = i & (TRACE_RING_SIZE - 1);
        struct trace_entry *e = &trace_ring[cpu][slot];
        const char *name =
            (e->fn_id < FN_NR && fn_names[e->fn_id]) ? fn_names[e->fn_id] : "?";

        kprintf("  [%2u] %-22s ctx=%08x ra=%lx arg=%lx\n", i - start, name,
                (unsigned) e->ctx, (unsigned long) e->ra,
                (unsigned long) e->arg);
    }
    kprintf("=== trace ring end ===\n");
}

#endif /* CONFIG_TRACE_RING */
