/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Debug-only fault injection (CONFIG_DEBUG_INJECT). One private call,
 * DEBUG_INJECT, gated by its own capability RTMUX_PRIV_CAP_INJECT, so tests can
 * produce a deterministic stall or allocation failure instead of waiting for a
 * race. With the option off the call is unbound (ENOSYS) and the code is not
 * built. See docs/capability-abi.md.
 */
#ifndef _PRIVATE_INJECT_H
#define _PRIVATE_INJECT_H

#include <stdatomic.h>
#include <stdint.h>

/* SPIN_IRQOFF(hart, ms): hart busy-waits ms (<= 10000) with interrupts off. */
#define INJECT_OP_SPIN_IRQOFF 1
/* HANG_HART(hart): hart busy-waits for ever with interrupts off. */
#define INJECT_OP_HANG_HART 2

/* SPIN_CONSOLE(hart, ms): like SPIN_IRQOFF, but hart holds the console lock for
 * the whole spin, so any other hart's ordinary kprintf waits (the fixture for
 * the watchdog's lock-free report).
 */
#define INJECT_OP_SPIN_CONSOLE 5

/* FAIL_NEXT_ALLOC(kstat_soul_id, pid): the next ordinary object_alloc of that
 * pool charged to that process fails once.
 */
#define INJECT_OP_FAIL_NEXT_ALLOC 3
/* ALLOC_STATUS: ret is 1 if the armed failure was consumed, else 0. */
#define INJECT_OP_ALLOC_STATUS 4

#define INJECT_SPIN_MAX_MS 10000

/* Injected interrupts-off spins, for intrevent_flush()'s debug spin watchdog: a
 * flush that waits on a deliberately stalled hart is the injected condition,
 * not a kernel bug, so it must not crash() while the stall lasts. One word
 * keeps the snapshot coherent: the high half counts spins ever started, the low
 * half harts now inside one (a hung hart never leaves). The spinning hart's IPI
 * handler starts a spin with inject_spin_begin() and ends it with
 * inject_spin_end(). A waiter takes inject_stall_token() and later asks
 * inject_stall_overlapped(): true if a spin was active at the token or any spin
 * began or ended since, even one that finished while the waiter was itself held
 * up.
 */
extern atomic_ullong inject_spin_state;

static inline void inject_spin_begin(void)
{
    atomic_fetch_add_explicit(&inject_spin_state, (1ULL << 32) + 1,
                              memory_order_relaxed);
}

static inline void inject_spin_end(void)
{
    atomic_fetch_sub_explicit(&inject_spin_state, 1, memory_order_relaxed);
}

static inline unsigned long long inject_stall_token(void)
{
    return atomic_load_explicit(&inject_spin_state, memory_order_relaxed);
}

static inline int inject_stall_overlapped(unsigned long long token0)
{
    return (token0 & 0xffffffffULL) != 0 || inject_stall_token() != token0;
}

struct kcall_args_debug_inject {
    unsigned op;
    uint64_t arg0;
    uint64_t arg1;
    int ret; /* 0 / EINVAL / ..., or the ALLOC_STATUS answer */
};

#endif
