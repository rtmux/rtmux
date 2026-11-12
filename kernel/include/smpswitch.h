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
#define SPINUNLOCK(spin) ((spin)->value = 0)
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

/* LEGAL_CPU_BITMASK — bitmask of all active hart IDs. Built at boot from
 * hart_ids[]. Cannot be a simple 0..N-1 formula because hart IDs may be
 * non-contiguous.
 */
extern unsigned legal_cpu_bitmask;
#define LEGAL_CPU_BITMASK legal_cpu_bitmask

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
