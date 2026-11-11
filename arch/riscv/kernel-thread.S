/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Cooperative S-mode continuation save for kernel workers.
 *
 * U-mode traps save directly into thp->reg, but S-mode traps return from a
 * transient stack frame.  A kernel worker that wants to leave through
 * kernel_exit() must refresh its saved context explicitly first.
 *
 * int kernel_thread_checkpoint(CPU_REGISTERS *regs)
 *   first return: 0
 *   return after __kernel_restore: 1
 */

#include <trap-asm.h>

    .section .text
    .align 4
    .option norvc
    .globl kernel_thread_checkpoint
    .type kernel_thread_checkpoint, @function

kernel_thread_checkpoint:
    sd      ra,  REG_RA(a0)
    sd      sp,  REG_SP(a0)
    sd      gp,  REG_GP(a0)
    sd      tp,  REG_TP(a0)
    sd      s0,  REG_S0(a0)
    sd      s1,  REG_S1(a0)
    sd      s2,  REG_S2(a0)
    sd      s3,  REG_S3(a0)
    sd      s4,  REG_S4(a0)
    sd      s5,  REG_S5(a0)
    sd      s6,  REG_S6(a0)
    sd      s7,  REG_S7(a0)
    sd      s8,  REG_S8(a0)
    sd      s9,  REG_S9(a0)
    sd      s10, REG_S10(a0)
    sd      s11, REG_S11(a0)

    li      t0, 1
    sd      t0, REG_A0(a0)
    sd      ra, REG_PC(a0)
    li      t0, (SSTATUS_SPP | SSTATUS_SPIE | SSTATUS_SUM)
    sd      t0, REG_SSTATUS(a0)

    li      a0, 0
    ret

    .size kernel_thread_checkpoint, . - kernel_thread_checkpoint
