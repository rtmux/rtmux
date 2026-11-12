/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Minimal machine/atomic.h for RTMux ldso (RISC-V)
 *
 * ldso is single-threaded during bootstrap; these are simple stubs.
 */
#ifndef _MACHINE_ATOMIC_H
#define _MACHINE_ATOMIC_H

#define atomic_store_rel_ptr(p, v) (*(volatile void **) (p) = (v))
#define atomic_load_acq_ptr(p) (*(volatile void **) (p))

#define atomic_store_rel_int(p, v) (*(volatile int *) (p) = (int) (v))
#define atomic_load_acq_int(p) (*(volatile int *) (p))

#define atomic_store_rel_32(p, v) (*(volatile uint32_t *) (p) = (uint32_t) (v))
#define atomic_load_acq_32(p) (*(volatile uint32_t *) (p))

#define atomic_store_rel_long(p, v) (*(volatile long *) (p) = (long) (v))
#define atomic_load_acq_long(p) (*(volatile long *) (p))

#define atomic_add_int(p, v) (*(volatile int *) (p) += (v))
#define atomic_subtract_int(p, v) (*(volatile int *) (p) -= (v))

#define atomic_cmpset_int(p, o, n)                      \
    ({                                                  \
        int __r = (*(volatile int *) (p) == (int) (o)); \
        if (__r) {                                      \
            *(volatile int *) (p) = (int) (n);          \
        }                                               \
        __r;                                            \
    })

#define atomic_testandset_int(p, b)                   \
    ({                                                \
        int __r = (*(volatile int *) (p) >> (b)) & 1; \
        *(volatile int *) (p) |= (1 << (b));          \
        __r;                                          \
    })

/* 32-bit explicit-width variants */
#define atomic_add_32(p, v) (*(volatile uint32_t *) (p) += (uint32_t) (v))

/* Acquire/release variants */
#define atomic_cmpset_acq_int(p, o, n) atomic_cmpset_int(p, o, n)
#define atomic_add_acq_int(p, v) atomic_add_int(p, v)
#define atomic_add_rel_int(p, v) atomic_add_int(p, v)

/* Fetch-and-add returning old value */
#define atomic_fetchadd_int(p, v)          \
    ({                                     \
        int __old = *(volatile int *) (p); \
        *(volatile int *) (p) += (v);      \
        __old;                             \
    })

#define atomic_fetchadd_32(p, v)                     \
    ({                                               \
        uint32_t __old = *(volatile uint32_t *) (p); \
        *(volatile uint32_t *) (p) += (v);           \
        __old;                                       \
    })

/* Cache line size */
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif

#define atomic_thread_fence_acq() __asm volatile("fence r, rw" ::: "memory")
#define atomic_thread_fence_rel() __asm volatile("fence rw, w" ::: "memory")
#define atomic_thread_fence_acq_rel() \
    __asm volatile("fence rw, rw" ::: "memory")
#define atomic_thread_fence_seq_cst() \
    __asm volatile("fence rw, rw" ::: "memory")

#endif
