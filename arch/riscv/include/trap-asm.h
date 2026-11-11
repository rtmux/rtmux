/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* trap-asm.h — Symbolic constants for RISC-V trap entry/exit assembly.
 *
 * Shared between trap.S, entry.S, context_switch.S, and C handlers. All values
 * are plain numbers (no UL suffix) so they work in both C and GNU as
 * expressions.
 *
 * License: Apache 2.0
 */

#ifndef _RISCV_TRAP_ASM_H
#define _RISCV_TRAP_ASM_H

/* RISCV_CPU_REGISTERS frame layout.
 *
 * Matches struct riscv_cpu_registers in context.h:
 *   gpr[0..31]  at offsets 0..248   (32 * 8 = 256 bytes)
 *   pc          at offset 256       (saved sepc)
 *   sstatus     at offset 264       (saved sstatus)
 *
 * Total: 272 bytes (34 * 8), naturally 16-byte aligned.
 *
 * Both U-mode and S-mode traps use this layout — one frame format, one
 * save/restore sequence, one C handler signature.
 */
#define REG_ZERO (0 * 8)     /* x0  — hardwired zero              */
#define REG_RA (1 * 8)       /* x1  — return address              */
#define REG_SP (2 * 8)       /* x2  — stack pointer               */
#define REG_GP (3 * 8)       /* x3  — global pointer              */
#define REG_TP (4 * 8)       /* x4  — thread pointer              */
#define REG_T0 (5 * 8)       /* x5  — temporary 0                 */
#define REG_T1 (6 * 8)       /* x6  — temporary 1                 */
#define REG_T2 (7 * 8)       /* x7  — temporary 2                 */
#define REG_S0 (8 * 8)       /* x8  — saved reg 0 / frame pointer */
#define REG_S1 (9 * 8)       /* x9  — saved reg 1                 */
#define REG_A0 (10 * 8)      /* x10 — arg 0 / retval 0            */
#define REG_A1 (11 * 8)      /* x11 — arg 1 / retval 1            */
#define REG_A2 (12 * 8)      /* x12 — arg 2                       */
#define REG_A3 (13 * 8)      /* x13 — arg 3                       */
#define REG_A4 (14 * 8)      /* x14 — arg 4                       */
#define REG_A5 (15 * 8)      /* x15 — arg 5                       */
#define REG_A6 (16 * 8)      /* x16 — arg 6                       */
#define REG_A7 (17 * 8)      /* x17 — arg 7 / syscall number      */
#define REG_S2 (18 * 8)      /* x18 — saved reg 2                 */
#define REG_S3 (19 * 8)      /* x19 — saved reg 3                 */
#define REG_S4 (20 * 8)      /* x20 — saved reg 4                 */
#define REG_S5 (21 * 8)      /* x21 — saved reg 5                 */
#define REG_S6 (22 * 8)      /* x22 — saved reg 6                 */
#define REG_S7 (23 * 8)      /* x23 — saved reg 7                 */
#define REG_S8 (24 * 8)      /* x24 — saved reg 8                 */
#define REG_S9 (25 * 8)      /* x25 — saved reg 9                 */
#define REG_S10 (26 * 8)     /* x26 — saved reg 10                */
#define REG_S11 (27 * 8)     /* x27 — saved reg 11                */
#define REG_T3 (28 * 8)      /* x28 — temporary 3                 */
#define REG_T4 (29 * 8)      /* x29 — temporary 4                 */
#define REG_T5 (30 * 8)      /* x30 — temporary 5                 */
#define REG_T6 (31 * 8)      /* x31 — temporary 6                 */
#define REG_PC (32 * 8)      /* saved program counter (sepc)      */
#define REG_SSTATUS (33 * 8) /* saved sstatus                     */

#define FRAME_SIZE 272 /* 34 * 8 = 272 bytes                */

/* Trap auxiliary fields — live immediately after RISCV_CPU_REGISTERS in
 * thread_t (thp->trap_aux). Accessed as sscratch + TAUX_xxx during U-mode trap
 * entry, where sscratch = &thp->reg.
 *
 * Populated by kernel_exit before every sret to U-mode.
 */
#define TAUX_KERNEL_SP                                    \
    (FRAME_SIZE + 0 * 8) /* 272: per-CPU kernel stack top \
                          */
#define TAUX_KERNEL_TP \
    (FRAME_SIZE + 1 * 8) /* 280: kernel TLS ptr (S-mode tp) */

/* TLS struct offset for the cpupage pointer.
 * _thread_local_storage.__reserved2[0] is at byte offset 88. Kernel code stores
 * cpupageptr[cpu] there at init and context switch.
 */
#define TLS_CPUPAGE 88

/* struct cpupage_entry offsets (must match internal/include/sys/syspage.h).
 *
 * In S-mode, tp always points to this CPU's cpupage_entry. spare[0..1] are used
 * as scratch slots during S-mode trap entry to save t5/t6 before the stack
 * frame is allocated. (U-mode traps save directly into thp->reg via sscratch.)
 */
#define CPUPAGE_TLS 0         /* tls pointer                       */
#define CPUPAGE_PLS 8         /* pls pointer                       */
#define CPUPAGE_SYSPAGE 16    /* syspage pointer                   */
#define CPUPAGE_CPU 24        /* cpu number                        */
#define CPUPAGE_STATE 32      /* state                             */
#define CPUPAGE_TRAPFRAME 40  /* trapframe VA (Sv39)               */
#define CPUPAGE_SCRATCH_T5 48 /* spare[0]: scratch for t5          */
#define CPUPAGE_SCRATCH_T6 56 /* spare[1]: scratch for t6          */
#define CPUPAGE_SCRATCH_SP 64 /* spare[2]: scratch for sp (U-mode) */

/* sstatus CSR bit definitions. Plain hex values — no UL suffix — safe for both
 * C and assembly.
 *
 * Note: cpu-inline.h defines these with (1UL << N) for C code. These
 * definitions are for assembly use; the values are identical.
 */
#ifdef __ASSEMBLY__
#define SSTATUS_SIE 0x2        /* Supervisor Interrupt Enable       */
#define SSTATUS_SPIE 0x20      /* Supervisor Previous Interrupt En. */
#define SSTATUS_SPP 0x100      /* Supervisor Previous Privilege     */
#define SSTATUS_FS_MASK 0x6000 /* Floating-point Status mask        */
#define SSTATUS_SUM 0x40000    /* S-mode User Memory access         */
#endif

/* scause interrupt bit (bit 63 on RV64). */
#define SCAUSE_INTERRUPT (1 << 63)

#endif /* _RISCV_TRAP_ASM_H */
