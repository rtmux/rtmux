/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* trapframe.h — Per-process Sv39 trapframe structure
 *
 * Allocated per-process, mapped at TRAPFRAME VA in the user's page table. Not
 * accessible from user mode (no PTE_U flag). The trampoline assembly
 * saves/restores user registers here and uses the kernel_* fields to switch to
 * the kernel address space.
 *
 * The register layout must match the offsets in trampoline.S.
 *
 * License: Apache 2.0
 */

#ifndef _RISCV_TRAPFRAME_H
#define _RISCV_TRAPFRAME_H

/* Offsets for assembly — these are pure arithmetic, safe for .S files */
#define TF_RA (1 * 8)           /* regs[1] = ra */
#define TF_SP (2 * 8)           /* regs[2] = sp */
#define TF_GP (3 * 8)           /* regs[3] = gp */
#define TF_TP (4 * 8)           /* regs[4] = tp */
#define TF_T0 (5 * 8)           /* regs[5] */
#define TF_T1 (6 * 8)           /* regs[6] */
#define TF_T2 (7 * 8)           /* regs[7] */
#define TF_S0 (8 * 8)           /* regs[8] = fp */
#define TF_S1 (9 * 8)           /* regs[9] */
#define TF_A0 (10 * 8)          /* regs[10] = a0 */
#define TF_A1 (11 * 8)          /* regs[11] = a1 */
#define TF_A2 (12 * 8)          /* regs[12] = a2 */
#define TF_A3 (13 * 8)          /* regs[13] = a3 */
#define TF_A4 (14 * 8)          /* regs[14] = a4 */
#define TF_A5 (15 * 8)          /* regs[15] = a5 */
#define TF_A6 (16 * 8)          /* regs[16] = a6 */
#define TF_A7 (17 * 8)          /* regs[17] = a7 */
#define TF_S2 (18 * 8)          /* regs[18] */
#define TF_S3 (19 * 8)          /* regs[19] */
#define TF_S4 (20 * 8)          /* regs[20] */
#define TF_S5 (21 * 8)          /* regs[21] */
#define TF_S6 (22 * 8)          /* regs[22] */
#define TF_S7 (23 * 8)          /* regs[23] */
#define TF_S8 (24 * 8)          /* regs[24] */
#define TF_S9 (25 * 8)          /* regs[25] */
#define TF_S10 (26 * 8)         /* regs[26] */
#define TF_S11 (27 * 8)         /* regs[27] */
#define TF_T3 (28 * 8)          /* regs[28] */
#define TF_T4 (29 * 8)          /* regs[29] */
#define TF_T5 (30 * 8)          /* regs[30] */
#define TF_T6 (31 * 8)          /* regs[31] */
#define TF_KERNEL_SATP (32 * 8) /* 256 */
#define TF_KERNEL_SP (33 * 8)   /* 264 */
#define TF_USERTRAP (34 * 8)    /* 272 */
#define TF_SEPC (35 * 8)        /* 280 */
#define TF_CPUPAGE (36 * 8)     /* 288: &cpupage for sscratch restore */

#ifndef __ASSEMBLY__

#include <inttypes.h>

struct sv39_trapframe {
    /* 0..255: saved user registers x0-x31 (x0 is always 0 but simplifies
     * indexing — 32 regs * 8 bytes = 256 bytes)
     */
    uint64_t regs[32];

    /* 256: kernel's SATP value (for switching to kernel page table) */
    uint64_t kernel_satp;

    /* 264: kernel stack pointer (for C trap handler) */
    uint64_t kernel_sp;

    /* 272: address of usertrap() C handler */
    uint64_t usertrap_addr;

    /* 280: saved user PC (sepc at time of trap) */
    uint64_t sepc;

    /* 288: &cpupage pointer (for sscratch restore in uservec) */
    uint64_t cpupage;
};

#endif /* !__ASSEMBLY__ */

#endif /* _RISCV_TRAPFRAME_H */
