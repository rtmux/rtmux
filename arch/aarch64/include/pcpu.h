/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Per-CPU kernel state. TPIDR_EL1 points at this CPU's entry throughout EL1
 * execution; TPIDR_EL0 is reserved for the current user thread's libc TLS.
 * pc_hart retains the common kernel field name and contains MPIDR_EL1.Aff0
 * (0..3 on Pi 3). Other CPUs access pcpu_array[cpu] directly.
 */

#ifndef _KERNEL_ARCH_AARCH64_PCPU_H
#define _KERNEL_ARCH_AARCH64_PCPU_H

/* Compile-time offset macros for use in inline asm AND .S files. Kept outside
 * the __ASSEMBLY__ guard so assembly sources see them. The C-side
 * _Static_assert below pins these to the actual struct layout — if a field is
 * reordered and an asm site silently uses the wrong offset, the build fails at
 * compile time.
 */
#define PC_CURTHREAD 0
#define PC_HART 8

#ifndef __ASSEMBLY__

#include <stddef.h>
#include <stdatomic.h>
#include <stdbool.h>

/* Forward declarations — struct tags only.  Real definitions in <objects.h>. */
struct thread_entry;   /* thread_t */
struct process_entry;  /* process_t */
struct dispatch_entry; /* kdispatch_t */
struct fault_handlers;

struct pcpu {
    /* hot fields (first cache line)
     *
     * pc_curthread MUST be offset 0. Don't reorder.
     */
    struct thread_entry *pc_curthread; /* offset 0  — current thread */
    unsigned pc_hart;                  /* offset 8  — get_cpunum() */
    unsigned pc_in_kcall;              /* offset 12 — was in_kcall[] */
    unsigned pc_reserved16;          /* offset 16, reserved (was preempt_count,
                                        primitive removed; kept as a slot so the
                                        following first-cache-line field offsets
                                        do not shift)
                                        */
    unsigned pc_in_specret;          /* offset 20 — was in_specret[] */
    uint8_t pc_pending_preempt_prio; /* was pending_preempt_prio[] */
    uint8_t pc_soul_alloc_override;  /* was soul_alloc_override[] */
    bool pc_alive;                   /* was alives[] */
    /* pc_kspin_depth — kspinlock nesting depth. Incremented after every
     * kspinlock_lock, decremented before every kspinlock_unlock. Only the
     * outermost unlock (depth 1 → 0) restores SIE via interrupt_enable. Without
     * this counter, an inner kspinlock_unlock would re-enable interrupts while
     * a still-held outer kspinlock expected them disabled — a SEXT firing in
     * the window between two unlocks could re-enter ready_default and
     * recursively self-deadlock on SCHED_LOCK_LOCAL. Capped at uint8_t (255) —
     * kernel hot paths nest at most ~3-4 deep.
     */
    uint8_t pc_kspin_depth;
    unsigned pc_in_kprintf;              /* was in_kprintf[] (kprintf.c) */
    struct process_entry *pc_as_prp;     /* offset 32 — was as_prp[] */
    struct process_entry *pc_active_prp; /* offset 40 — was actives_prp[] */
    struct thread_entry *pc_idle_thp;    /* offset 48 — was idle_thp[] */
    uintptr_t pc_ksys_stack;             /* offset 56 — was ksys_stack[] */

    /* second cache line */
    uint32_t pc_rcu_qs;           /* RCU-sched QS epoch */
    unsigned pc_rcu_waiting;      /* RCU-sched waiter state */
    int pc_syscall;               /* current syscall, or -1 */
    atomic_uint pc_ipicmds;       /* was ipicmds[] (cross-hart) */
    atomic_uint pc_tlb_flush_req; /* requested remote TLB invalidation ticket */
    atomic_uint pc_tlb_flush_done; /* highest acked TLB invalidation ticket */
    uint32_t pc_ipi_handler_count; /* was ipi_handler_count[] */
    struct dispatch_entry *pc_dispatch; /* was percpu_dispatch[] */
    const struct fault_handlers
        *volatile pc_xfer_handlers;     /* was xfer_handlers[] */
    struct thread_entry *pc_active_fpu; /* was actives_fpu[] */
    struct thread_entry *pc_active_pcr; /* was actives_pcr[] */
    uint64_t pc_clockcycles_offset;     /* was clockcycles_offset[] */

    /* diagnostics (cold) */
    uint64_t pc_last_kernel_pc; /* was last_kernel_pc[] */
    uint64_t pc_last_scause;    /* was last_scause[] */
    volatile uint32_t
        pc_inject_spin_ms; /* IPI_DEBUG_SPIN: ms, UINT32_MAX = hang */
    volatile uint32_t pc_inject_hold_console; /* IPI_DEBUG_SPIN: spin under the
                                                 console lock */

    /* CPU-time billing (sched_account_current in kernel/sched/dispatch.c).
     * pc_account_stamp is the snap_time() ns at which the active thread's
     * unbilled stretch began. Written only by the owner hart; pc_account_seq is
     * the seqcount (odd = update in flight) that lets another hart's clock read
     * add the live stretch to a running thread's banked running_time.
     */
    uint64_t pc_account_stamp;
    atomic_uint pc_account_seq;
    uint8_t pc_account_billed; /* active thread already billed before enqueue */

    /* pc_timer_progress — supervisor timer interrupts this hart has taken.
     * Written only by the owner hart (relaxed fetch_add at timer-trap entry,
     * both the in-kernel and full paths); read by the neighbour-watchdog hart
     * (kernel/debug/hartwatch.c) with a relaxed load. No ordering is needed:
     * the reader only asks "did the value change", and a stale read delays a
     * report by one tick. Cross-hart rule: single writer, any reader.
     */
    atomic_ulong pc_timer_progress;
    uint8_t pc_pad[72]; /* keep sizeof(struct pcpu) power-of-two */
};

#ifndef __cplusplus
_Static_assert(offsetof(struct pcpu, pc_curthread) == PC_CURTHREAD,
               "pc_curthread must be at offset 0");
_Static_assert(offsetof(struct pcpu, pc_hart) == PC_HART,
               "pc_hart offset mismatch — update PC_HART macro");
_Static_assert(offsetof(struct pcpu, pc_rcu_qs) >= 64,
               "RCU-sched fields must not share the first pcpu cache line");
_Static_assert(offsetof(struct pcpu, pc_rcu_waiting) < 128,
               "RCU-sched fields must stay in the second pcpu cache line");
/* Power-of-2 sizing lets _smpstart (arch/aarch64/cpu-misc.c) index pcpu_array
 * with a single "slli + add" using __builtin_ctz(sizeof). If the struct grows
 * past the next power of 2 (current is 256), add padding to restore the
 * invariant or switch the AP boot asm to "mul".
 */
_Static_assert((sizeof(struct pcpu) & (sizeof(struct pcpu) - 1)) == 0,
               "sizeof(struct pcpu) must be a power of 2 for AP boot asm");
#endif

/* Storage: one slot per hart, indexed by hart ID. Definition lives in
 * kernel/core/globals.c. Allocated with PROCESSORS_MAX slots even though only
 * num_processors are live — the unused slots are zero (BSS) and unreferenced.
 *
 * Use:
 *   - self-hart:   get_pcpu()               (reads tp, no chain)
 *   - cross-hart:  &pcpu_array[remote_hart]
 *
 * The get_pcpu() accessor is defined in <arch/runtime.h>.
 */
extern struct pcpu pcpu_array[];

#endif /* __ASSEMBLY__ */

#endif /* _KERNEL_ARCH_AARCH64_PCPU_H */
