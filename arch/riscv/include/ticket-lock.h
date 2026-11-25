/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_RISCV_TICKET_LOCK_H
#define RTMUX_RISCV_TICKET_LOCK_H

#include <sys/spinlock.h>

/* Little-endian RV64 packs the next ticket above the serving ticket. The
 * owner alone writes the low halfword. Advancing it cannot carry into the
 * high half, even as another hart reserves a ticket with amoadd.w. */
static inline unsigned intrspin_ticket(struct intrspin *spin)
{
    unsigned old = __atomic_fetch_add(&spin->value, 1u << 16, __ATOMIC_ACQUIRE);
    unsigned ticket = old >> 16;
    if ((unsigned short) old != ticket) {
        while ((unsigned short) __atomic_load_n(&spin->value, __ATOMIC_RELAXED) != ticket)
            __asm__ volatile("" ::: "memory");
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    }
    return ticket;
}

static inline void intrspin_acquire(struct intrspin *spin)
{
    (void) intrspin_ticket(spin);
}

static inline int intrspin_is_locked(const struct intrspin *spin)
{
    unsigned word = __atomic_load_n(&spin->value, __ATOMIC_RELAXED);
    return (unsigned short) word != (word >> 16);
}

static inline void intrspin_release(struct intrspin *spin)
{
    unsigned owner = __atomic_load_n(&spin->value, __ATOMIC_RELAXED) + 1u;
    typedef unsigned short owner_half __attribute__((__may_alias__));
    _Static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ &&
                   sizeof(unsigned) == 4 && sizeof(owner_half) == 2,
                   "ticket owner must be the low-address halfword");
    __atomic_store_n((volatile owner_half *) &spin->value, (owner_half) owner,
                     __ATOMIC_RELEASE);
}

#endif
