/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Define various SMP specific macros that are shared between the kernel and
 * proc
 */

/* PROCESSORS_MAX is always CONFIG_PROCESSORS_MAX (from Kconfig). */
#define PROCESSORS_MAX CONFIG_PROCESSORS_MAX

extern unsigned num_processors;

#if CONFIG_PROCESSORS_MAX > 1
#define WANT_SMP_MACROS
#endif

#if defined(WANT_SMP_MACROS)

#define SPINLOCK(spin)                              \
    do {                                            \
        while ((spin)->value) { /* nothing to do */ \
        }                                           \
    } while (_smp_xchg(&(spin)->value, 1) != 0)
/* Publish critical-section writes before the next acquire exchange. */
#define SPINUNLOCK(spin) __atomic_store_n(&(spin)->value, 0, __ATOMIC_RELEASE)
#define SENDIPI(cpu, cmd) send_ipi(cpu, cmd)
#define SMP_FLUSH_TLB()     \
    if (num_processors > 1) \
    smp_flush_tlb()
#define SMP_SPINVAR(class, var) class intrspin_t var
#define INTR_LOCK(s) interrupt_lock(s)

/* the following ensure we don't enable interrupts during kernel initialization
 */
#define INTR_UNLOCK(s) interrupt_unlock(s)
#define MEM_BARRIER_RD() __cpu_membarrier()
#define MEM_BARRIER_WR() __cpu_membarrier()
#define MEM_BARRIER_RW() __cpu_membarrier()

#else /* Single-CPU */
#define SPINLOCK(spin)
#define SPINUNLOCK(spin)
#define SENDIPI(cpu, cmd)
#define SMP_FLUSH_TLB()
#define SMP_SPINVAR(class, var)
#define INTR_LOCK(s) interrupt_disable()

/* the following ensure we don't enable interrupts during kernel initialization
 */
#define INTR_UNLOCK(s) interrupt_enable()
#define MEM_BARRIER_RD()
#define MEM_BARRIER_WR()
#define MEM_BARRIER_RW()
#endif

/* LEGAL_CPU_BITMASK — bitmask of the logical CPUs that were not dropped: set to
 * 0..num_processors-1 at boot; kext_smp_bringup() clears the bit of a CPU whose
 * hart does not start. 64 bits wide, so PROCESSORS_MAX may not exceed 64 (the
 * runmask ABI caps it at 32 today, see kernel/sched/thread.c).
 */
extern uint64_t legal_cpu_bitmask;
_Static_assert(CONFIG_PROCESSORS_MAX <= 64,
               "LEGAL_CPU_BITMASK holds at most 64 logical CPUs");
#define LEGAL_CPU_BITMASK legal_cpu_bitmask

/* online_cpu_mask — the logical CPUs that have joined and are running: the boot
 * CPU from kernel init, an AP when it publishes pc_alive (cpu_idle), and never
 * a hart that failed to start or arrived late. The bit is set just before
 * pc_alive, so whoever waits for pc_alive finds it set. Every loop that visits
 * other CPUs, signals one or waits for one tests cpu_online();
 * LEGAL_CPU_BITMASK differs only in still holding an AP that has not joined
 * yet, which the scheduler may queue a thread for. send_ipi() refuses a CPU
 * outside LEGAL_CPU_BITMASK and counts the attempt in pc_ipi_refused.
 */
extern uint64_t online_cpu_mask;

#ifndef CPU_ONLINE_DEFINED /* this header has no include guard */
#define CPU_ONLINE_DEFINED

/* The mask once, for a loop over the CPUs: test each CPU against the copy with
 * cpu_in_mask() instead of an acquire load per CPU. A CPU that joins during the
 * loop is missed by it, as it would be by a test just before the join.
 */
static inline uint64_t cpu_online_snapshot(void)
{
    return __atomic_load_n(&online_cpu_mask, __ATOMIC_ACQUIRE);
}

static inline int cpu_in_mask(uint64_t mask, unsigned cpu)
{
    return cpu < 64 && (mask >> cpu) & 1;
}

static inline int cpu_online(unsigned cpu)
{
    return cpu_in_mask(cpu_online_snapshot(), cpu);
}
#endif

/* IPI commands for the SMP kernel. */
#define IPI_RESCHED 0x00000001
#define IPI_TIMESLICE 0x00000002
#define IPI_TLB_FLUSH 0x00000004
#define IPI_TLB_SAFE 0x00000008
#define IPI_CONTEXT_SAVE 0x00000010
#define IPI_CHECK_INTR 0x00000020
#define IPI_PARKIT 0x00000040
#define IPI_INTR_UNMASK 0x00000080
#define IPI_INTR_MASK 0x00000100
#define IPI_CLOCK_LOAD 0x00000200
#define IPI_DEBUG_SPIN 0x00000800 /* CONFIG_DEBUG_INJECT: busy-wait irq-off */
#define IPI_CRASH 0x00000400 /* sibling crash — quiesce + join rendezvous */
#define IPI_CPU_SPECIFIC 0xffff0000
