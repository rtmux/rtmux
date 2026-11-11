/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RISCV_CPU_DEF_H
#define _RISCV_CPU_DEF_H

#include <macros.h>
#include <arch/cpu.h>
#include <arch/smpxchg.h>

#define FAULT_ISWRITE(_n) ((_n) & SIGCODE_STORE)

typedef RISCV_CPU_REGISTERS CPU_REGISTERS;
typedef RISCV_FPU_REGISTERS FPU_REGISTERS;
typedef RISCV_ALT_REGISTERS ALT_REGISTERS;
typedef RISCV_PERF_REGISTERS PERF_REGISTERS;
#define CPU_STRINGNAME RISCV_STRINGNAME

#define STARTUP_STACK_NBYTES 16384
#define KSYS_STACK_NBYTES 16384
#define IDLE_STACK_NBYTES 4096
#define DEF_MAX_THREAD_STACKSIZE 32768

#define DEF_VIRTUAL_THREAD_STACKSIZE 524288
#define DEF_PHYSICAL_THREAD_STACKSIZE 4096
#define DEF_VIRTUAL_FIRST_THREAD_STACKSIZE 524288
#define DEF_PHYSICAL_FIRST_THREAD_STACKSIZE 4096

/* For RV64 */
#define VM_USER_SPACE_BOUNDARY (GIG(128UL) - 1)
#define VM_KERN_SPACE_BOUNDARY 0xffffffffffffffff

#define STACK_ALIGNMENT 16UL
#define STACK_INITIAL_CALL_CONVENTION_USAGE 0

#include <paging.h>

#define CPU_P2V(p) ((uintptr_t) (p) + KERNEL_VIRT_BASE)
#define CPU_P2V_NOCACHE(p) CPU_P2V(p)
#define CPU_V2P(v) ((paddr_t) ((uintptr_t) (v) - KERNEL_VIRT_BASE))
#define CPU_VADDR_IN_RANGE(v) ((uintptr_t) (v) >= KERNEL_VIRT_BASE)

/* registers that need to be saved for interrupt routine invocation */
struct cpu_intrsave {
    char dummy;
};

/* Breakpoint fields that this cpu may need for breakpoints */
#define CPU_DEBUG_BRKPT \
    paddr_t paddr;      \
    uint32_t org_val;

/* Fields this cpu may need for debugging a process */
#define CPU_DEBUG breakpoint_t step;

/* CPU specific fields in the thread entry structure */
struct cpu_thread_entry {
    RISCV_PERF_REGISTERS *pcr;
    struct riscv_alt_registers alt; /* WARNING: must be last field */
};

/* CPU specific fields in the vm_ops fault_info structure */
struct cpu_fault_info {
    unsigned code;
    unsigned asid;
};

/* Extra state (read only) made available by the kernel debugger */
struct cpu_extra_state {
    char dummy;
};

extern void mmu_set_domain(unsigned);

#endif
