/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _KERNEL_RCU_H
#define _KERNEL_RCU_H

#include <stdint.h>
#include <cpu-inline.h>

#ifndef __ASSEMBLY__

typedef struct rcu {
    const char *name;
} rcu_t;
typedef uint32_t rcu_epoch_t;
typedef int32_t rcu_epoch_delta_t;

struct rcu_sched_busy {
    uint64_t enroll_cycles;
    uint64_t scan_cycles;
    uint64_t post_scan_cycles;
    unsigned peers;
    unsigned thread;
    unsigned msg;
    unsigned sys;
    unsigned channel;
    unsigned connect;
    unsigned interrupt;
    unsigned sync;
    unsigned sched;
    unsigned time;
    unsigned idle;
    unsigned dispatch;
    unsigned timer;
    unsigned fault;
    unsigned priv;
    unsigned heartbeat;
    unsigned priv_system;
    unsigned priv_vm;
    unsigned priv_ipc;
    unsigned priv_process;
    unsigned priv_thread;
    unsigned priv_pa;
    unsigned priv_pa_alloc;
    unsigned priv_pa_free;
    unsigned priv_pa_zero;
    unsigned priv_pa_info;
    unsigned priv_pa_xlat;
    unsigned priv_pa_pq;
    unsigned unstamped;
    unsigned other;
};

#define RCU_EPOCH_INIT ((rcu_epoch_t) 0u)
#define RCU_EPOCH_INCR 2u
#define RCU_EPOCH_GEQ(a, b) ((rcu_epoch_delta_t) ((a) - (b)) >= 0)

#define RCU_INITIALIZER(nm) {.name = (nm)}

void rcu_sched_quiescent(void);
rcu_epoch_t rcu_sched_advance(void);
void rcu_sched_wait(rcu_epoch_t goal);
int rcu_sched_synchronize_detail(struct rcu_sched_busy *busy);
int rcu_sched_synchronize(void);

static inline void rcu_sched_read_lock(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void rcu_sched_read_unlock(void)
{
    __asm__ volatile("" ::: "memory");
}

static inline void rcu_read_lock(rcu_t *rcu)
{
    (void) rcu;
    rcu_sched_read_lock();
}

static inline void rcu_read_unlock(rcu_t *rcu)
{
    (void) rcu;
    rcu_sched_read_unlock();
}

#define rcu_dereference(ptr)               \
    ({                                     \
        __typeof__(ptr) _rcu_ptr = (ptr);  \
        __asm__ volatile("" ::: "memory"); \
        _rcu_ptr;                          \
    })

#define rcu_assign_pointer(ptr, val)                  \
    do {                                              \
        __asm__ volatile("fence rw, w" ::: "memory"); \
        (ptr) = (val);                                \
    } while (0)

#endif /* !__ASSEMBLY__ */
#endif /* _KERNEL_RCU_H */
