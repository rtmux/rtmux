/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/calls.h>
#include <trace/events.h>
#include "rcu.h"

static volatile rcu_epoch_t rcu_sched_epoch = RCU_EPOCH_INIT;

rcu_t process_rcu = RCU_INITIALIZER("process_rcu");
rcu_t tthread_rcu = RCU_INITIALIZER("tthread_rcu");
rcu_t connobj_rcu = RCU_INITIALIZER("connobj_rcu");

static inline void rcu_cpu_relax(void)
{
    /* RISC-V Zihintpause PAUSE encoding, kept raw for older assemblers. */
    __asm__ volatile(".word 0x0100000f" ::: "memory");
}

static inline int rcu_cpu_alive(unsigned cpu)
{
    return __atomic_load_n(&pcpu_array[cpu].pc_alive, __ATOMIC_ACQUIRE);
}

void rcu_sched_quiescent(void)
{
    unsigned cpu = get_cpunum();
    rcu_epoch_t epoch = __atomic_load_n(&rcu_sched_epoch, __ATOMIC_RELAXED);

    if (__atomic_load_n(&pcpu_array[cpu].pc_rcu_qs, __ATOMIC_RELAXED) == epoch)
        return;
    __atomic_store_n(&pcpu_array[cpu].pc_rcu_qs, epoch, __ATOMIC_RELEASE);
    __asm__ volatile("" ::: "memory");
}

rcu_epoch_t rcu_sched_advance(void)
{
    rcu_epoch_t old;

    old = (rcu_epoch_t) atomic_add_value((volatile unsigned *) &rcu_sched_epoch,
                                         RCU_EPOCH_INCR);
    return old + RCU_EPOCH_INCR;
}

static void rcu_sched_busy_add(struct rcu_sched_busy *detail, int syscall)
{
    if (!detail)
        return;

    if (syscall == IN_KCALL_PHASE_NONE) {
        detail->unstamped++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_DISPATCH) {
        detail->dispatch++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_TIMER) {
        detail->timer++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_FAULT) {
        detail->fault++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV) {
        detail->priv++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_HEARTBEAT) {
        detail->heartbeat++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_SYSTEM) {
        detail->priv_system++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_VM) {
        detail->priv_vm++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_IPC) {
        detail->priv_ipc++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PROCESS) {
        detail->priv_process++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_THREAD) {
        detail->priv_thread++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PA) {
        detail->priv_pa++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PA_ALLOC) {
        detail->priv_pa_alloc++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PA_FREE) {
        detail->priv_pa_free++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PA_ZERO) {
        detail->priv_pa_zero++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PA_INFO) {
        detail->priv_pa_info++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PA_XLAT) {
        detail->priv_pa_xlat++;
        return;
    }
    if (syscall == IN_KCALL_PHASE_PRIV_PA_PQ) {
        detail->priv_pa_pq++;
        return;
    }
    syscall = _TRACE_GETSYSCALL(syscall);
    if (syscall == __KSYS_PRIV) {
        detail->priv++;
    } else if (syscall >= __KSYS_THREAD_CREATE &&
               syscall <= __KSYS_THREAD_CTL) {
        detail->thread++;
    } else if (syscall >= __KSYS_MSG_CURRENT &&
               syscall <= __KSYS_MSG_VERIFY_EVENT) {
        detail->msg++;
    } else if (syscall >= __KSYS_NOP && syscall <= __KSYS_SYS_SPARE1) {
        detail->sys++;
    } else if (syscall >= __KSYS_CHANNEL_CREATE &&
               syscall <= __KSYS_CHANNEL_SPARE1) {
        detail->channel++;
    } else if (syscall >= __KSYS_CONNECT_ATTACH &&
               syscall <= __KSYS_CONNECT_SPARE2) {
        detail->connect++;
    } else if (syscall >= __KSYS_INTERRUPT_ATTACH_THREAD &&
               syscall <= __KSYS_INTERRUPT_SPARE4) {
        detail->interrupt++;
    } else if (syscall >= __KSYS_SYNC_CREATE &&
               syscall <= __KSYS_SYNC_MUTEX_REVIVE) {
        detail->sync++;
    } else if (syscall >= __KSYS_SCHED_GET && syscall <= __KSYS_SCHED_INFO) {
        detail->sched++;
    } else if ((syscall >= __KSYS_CLOCK_TIME && syscall <= __KSYS_CLOCK_ID) ||
               (syscall >= __KSYS_TIMER_CREATE &&
                syscall <= __KSYS_TIMER_TIMEOUT)) {
        detail->time++;
    } else {
        detail->other++;
    }
}

static void rcu_sched_busy_add_peer(struct rcu_sched_busy *detail, unsigned cpu)
{
    struct thread_entry *cur;

    if (!detail)
        return;
    cur = __atomic_load_n(&pcpu_array[cpu].pc_curthread, __ATOMIC_ACQUIRE);
    if (cur == pcpu_array[cpu].pc_idle_thp) {
        detail->idle++;
        return;
    }
    rcu_sched_busy_add(
        detail, __atomic_load_n(&pcpu_array[cpu].pc_syscall, __ATOMIC_ACQUIRE));
}

static unsigned rcu_sched_busy_peers(struct rcu_sched_busy *detail)
{
    extern unsigned hart_ids[];
    unsigned self = get_cpunum();
    unsigned peers = 0;

    for (int i = 0; i < (int) num_processors; i++) {
        unsigned cpu = hart_ids[i];
        unsigned depth;

        if (cpu == self)
            continue;
        depth = __atomic_load_n(&in_kcall[cpu], __ATOMIC_ACQUIRE);
        if (depth == 0)
            continue;
        if (__atomic_load_n(&pcpu_array[cpu].pc_rcu_waiting,
                            __ATOMIC_ACQUIRE) != 0)
            continue;
        if (!rcu_cpu_alive(cpu))
            continue;
        peers++;
        rcu_sched_busy_add_peer(detail, cpu);
    }
    if (detail)
        detail->peers = peers;
    return peers;
}

void rcu_sched_wait(rcu_epoch_t goal)
{
    extern unsigned hart_ids[];
    unsigned self = get_cpunum();

    for (int i = 0; i < (int) num_processors; i++) {
        unsigned cpu = hart_ids[i];
        unsigned depth;

        if (cpu == self)
            continue;
        for (;;) {
            if (__atomic_load_n(&pcpu_array[cpu].pc_rcu_waiting,
                                __ATOMIC_RELAXED) != 0 &&
                __atomic_load_n(&pcpu_array[cpu].pc_rcu_waiting,
                                __ATOMIC_ACQUIRE) != 0)
                break;
            if (!rcu_cpu_alive(cpu))
                break;
            depth = __atomic_load_n(&in_kcall[cpu], __ATOMIC_RELAXED);
            if (depth == 0 &&
                __atomic_load_n(&in_kcall[cpu], __ATOMIC_ACQUIRE) == 0)
                break;
            if (RCU_EPOCH_GEQ(__atomic_load_n(&pcpu_array[cpu].pc_rcu_qs,
                                              __ATOMIC_RELAXED),
                              goal) &&
                RCU_EPOCH_GEQ(__atomic_load_n(&pcpu_array[cpu].pc_rcu_qs,
                                              __ATOMIC_ACQUIRE),
                              goal))
                break;
            rcu_cpu_relax();
        }
    }
    cpu_smp_mb();
}

int rcu_sched_synchronize_detail(struct rcu_sched_busy *busy)
{
    unsigned busy_peers;
    uint64_t t0 = 0;
    uint64_t t1 = 0;
    uint64_t t2 = 0;
    uint64_t t3 = 0;

    if (busy)
        *busy = (struct rcu_sched_busy){0};
#ifdef CONFIG_LOCK_DEBUG
    if (lock_debug_held_depth() != 0) {
        kprintf("RCU-sched: rcu_sched_synchronize while holding a kspinlock\n");
        crash();
    }
#endif
    if (busy)
        t0 = cpu_clock_ticks();
    /* Publish local enrollment before any skip/advance decision. */
    __atomic_store_n(&pcpu_array[get_cpunum()].pc_rcu_waiting, 1,
                     __ATOMIC_RELEASE);
    cpu_smp_mb();
    if (busy)
        t1 = cpu_clock_ticks();
    busy_peers = rcu_sched_busy_peers(busy);
    if (busy)
        t2 = cpu_clock_ticks();
    if (busy_peers) {
        rcu_epoch_t goal = rcu_sched_advance();

        rcu_sched_wait(goal);
    }
    if (busy) {
        t3 = cpu_clock_ticks();
        busy->enroll_cycles = t1 - t0;
        busy->scan_cycles = t2 - t1;
        busy->post_scan_cycles = t3 - t2;
    }
    __atomic_store_n(&pcpu_array[get_cpunum()].pc_rcu_waiting, 0,
                     __ATOMIC_RELEASE);
    return (int) busy_peers;
}

int rcu_sched_synchronize(void)
{
    return rcu_sched_synchronize_detail(NULL);
}
