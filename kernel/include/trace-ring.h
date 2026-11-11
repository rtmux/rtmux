/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * trace-ring.h — Per-CPU function-entry trace ring buffer.
 *
 * Lockless single-writer ring; each CPU writes only its own slots. Used for
 * systematically tracing the function-call sequence that leads up to a kernel
 * crash, particularly for SMP timing bugs that are hard to reproduce in a
 * debugger.
 *
 * Usage:
 *   - Annotate suspect functions with TRACE_FN(FN_xxx, ctx_value).
 *   - On crash, trace_ring_dump(cpu) prints the last entries for
 *     the trapping CPU.
 *
 * Defined in kernel/trace/ring.c.
 *
 * Off by default. CONFIG_TRACE_RING makes TRACE_FN record entries and
 * trace_ring_dump print them while chasing a bug.
 *
 * License: Apache 2.0
 */

#ifndef KERNEL_TRACE_RING_H
#define KERNEL_TRACE_RING_H

#include <stdint.h>
#include <smpswitch.h> /* CONFIG_PROCESSORS_MAX */

/* Function identifiers. Add new entries at the end; do not renumber existing
 * ones (saved trace dumps would become uninterpretable).
 */
enum trace_fn_id {
    FN_NONE = 0,
    FN_KCALL_DISPATCH = 1,
    FN_KCALL_NO_BKL_PRE = 2,
    FN_KCALL_NO_BKL_POST = 3,
    FN_KER_MSG_RECEIVEV = 4,
    FN_KER_MSG_RECV_BLOCK = 5,
    FN_KER_MSG_RECV_RET = 6,
    FN_KER_MSG_SENDV = 7,
    FN_KER_MSG_REPLYV = 8,
    FN_UNREADY = 9,
    FN_BLOCK = 10,
    FN_BLOCK_AND_READY = 11,
    FN_MARK_RUNNING = 12,
    FN_READY = 13,
    FN_FORCE_READY = 14,
    FN_INTREVENT_DRAIN = 15,
    FN_INTREVENT_ADD = 16,
    FN_STRAP_HANDLER_C = 17,
    FN_TRAP_TIMER_MIN = 18,
    FN_TRAP_TIMER_FULL = 19,
    FN_IPI_HANDLER = 20,
    FN_TRAP_PLIC = 21,
    FN_TRAP_ECALL_U = 22,
    FN_TRAP_ECALL_S = 23,
    FN_KER_EXIT = 24,
    FN_KER_RESTORE_PRE = 25,
    FN_ACQUIRE_KERNEL = 26,
    FN_RELEASE_KERNEL = 27,
    FN_KER_PRIV = 28,
    FN_KER_PRIV_RET = 29,
    FN_NR
};

#ifdef CONFIG_TRACE_RING

#define TRACE_RING_SIZE 32 /* power-of-two for cheap modulo */

struct trace_entry {
    uint8_t fn_id; /* enum trace_fn_id */
    uint8_t cpu;   /* verifier — must match position in array */
    uint16_t reserved;
    uint32_t ctx; /* function-specific context (e.g. tid) */
    uint64_t ra;  /* __builtin_return_address(0) at entry */
    uint64_t arg; /* user-supplied (e.g. pointer or value) */
};

extern struct trace_entry trace_ring[CONFIG_PROCESSORS_MAX][TRACE_RING_SIZE];
extern unsigned trace_idx[CONFIG_PROCESSORS_MAX];

extern void trace_ring_dump(unsigned cpu);

/* TRACE_FN — record entry to function with the given id. ctx_val: 32-bit
 * context (e.g. (uint32_t)act->tid). arg_val: 64-bit user-supplied (e.g.
 * (uint64_t)act, or 0).
 *
 * Lockless: each CPU writes its own slot; no other CPU touches this CPU's
 * slots. trace_idx is per-CPU so the index increment is per-CPU and never
 * contended.
 *
 * No interrupt-disable wrap: kspinlocks already disable IRQs around critical
 * sections; outside those, a same-CPU interrupt that pre-empts mid-store will,
 * on resume, see the next slot already advanced — at worst we lose one entry,
 * never corrupt the ring.
 */
extern unsigned get_cpunum(void);
#define TRACE_FN(id, ctx_val, arg_val)                               \
    do {                                                             \
        unsigned __cpu = get_cpunum();                               \
        if (__cpu < CONFIG_PROCESSORS_MAX) {                         \
            unsigned __i = trace_idx[__cpu] & (TRACE_RING_SIZE - 1); \
            struct trace_entry *__e = &trace_ring[__cpu][__i];       \
            __e->fn_id = (uint8_t) (id);                             \
            __e->cpu = (uint8_t) __cpu;                              \
            __e->ctx = (uint32_t) (ctx_val);                         \
            __e->ra = (uint64_t) __builtin_return_address(0);        \
            __e->arg = (uint64_t) (arg_val);                         \
            trace_idx[__cpu]++;                                      \
        }                                                            \
    } while (0)

#else /* !CONFIG_TRACE_RING */

#define TRACE_FN(id, ctx_val, arg_val) \
    do {                               \
    } while (0)
static inline void trace_ring_dump(unsigned cpu)
{
    (void) cpu;
}

#endif /* CONFIG_TRACE_RING */

#endif /* KERNEL_TRACE_RING_H */
