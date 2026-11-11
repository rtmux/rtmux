/*
 * trap.S — Unified RISC-V S-mode trap entry and exit.
 *
 * __strap_entry is set as stvec.  ALL S-mode traps — interrupts,
 * ecalls, exceptions — land here.
 *
 * sscratch protocol:
 *   U-mode running: sscratch = &thp->reg  (nonzero, kernel pointer)
 *   S-mode running: sscratch = 0
 *
 * On trap entry, csrrw swaps sscratch with t6:
 *   If old sscratch != 0 → came from U-mode, t6 = &thp->reg
 *   If old sscratch == 0 → came from S-mode (nested trap)
 *
 * U-mode path: saves registers DIRECTLY into thp->reg (no copy!).
 *   Kernel sp and tp are loaded from thp->trap_aux fields.
 *   This eliminates the actives[] lookup and act->reg = *regs copy
 *   that caused register corruption bugs.
 *
 * S-mode path: saves registers on current kernel stack (nested frame).
 *   tp already holds &cpupage (kernel convention).
 *
 * License: Apache 2.0
 */

#include <trap-asm.h>

    .section .text
    .align 4
    .option norvc
    .globl __strap_entry

/*
 * __strap_entry — S-mode trap entry point (stvec target).
 *
 * On entry, hardware has:
 *   sepc        = interrupted PC
 *   scause      = trap cause
 *   stval       = fault address (if applicable)
 *   sstatus.SPP = previous privilege (0=U, 1=S)
 *   sstatus.SIE = 0 (interrupts disabled)
 *   All 31 GPRs unchanged from interrupted code.
 */
__strap_entry:
    /*
     * Swap t6 with sscratch.  After this:
     *   t6 = old sscratch value
     *   sscratch = interrupted code's t6
     *
     * If t6 == 0 → trap from S-mode (sscratch was 0)
     * If t6 != 0 → trap from U-mode (sscratch was &thp->reg)
     */
    csrrw   t6, sscratch, t6
    beqz    t6, .Lsmode_save

/* ================================================================ */
/* U-MODE TRAP PATH                                                 */
/* Save all registers directly into thp->reg (t6 = &thp->reg).     */
/* No kernel stack frame needed — thp->reg IS the save area.        */
/* ================================================================ */

    /* Save t5 first (need it as scratch) */
    sd      t5, REG_T5(t6)

    /* Recover user t6 from sscratch and zero sscratch atomically.
     * After this: t5 = user's original t6, sscratch = 0. */
    csrrw   t5, sscratch, zero
    sd      t5, REG_T6(t6)

    /* sscratch = 0: S-mode convention now established. */

    /* Save user sp, switch to per-CPU kernel stack */
    sd      sp, REG_SP(t6)
    ld      sp, TAUX_KERNEL_SP(t6)

    /* Save user tp, switch to per-CPU cpupage pointer */
    sd      tp, REG_TP(t6)
    ld      tp, TAUX_KERNEL_TP(t6)

    /* Save all remaining user GPRs directly into thp->reg */
    sd      ra,  REG_RA(t6)
    sd      gp,  REG_GP(t6)
    sd      t0,  REG_T0(t6)
    sd      t1,  REG_T1(t6)
    sd      t2,  REG_T2(t6)
    sd      s0,  REG_S0(t6)
    sd      s1,  REG_S1(t6)
    sd      a0,  REG_A0(t6)
    sd      a1,  REG_A1(t6)
    sd      a2,  REG_A2(t6)
    sd      a3,  REG_A3(t6)
    sd      a4,  REG_A4(t6)
    sd      a5,  REG_A5(t6)
    sd      a6,  REG_A6(t6)
    sd      a7,  REG_A7(t6)
    sd      s2,  REG_S2(t6)
    sd      s3,  REG_S3(t6)
    sd      s4,  REG_S4(t6)
    sd      s5,  REG_S5(t6)
    sd      s6,  REG_S6(t6)
    sd      s7,  REG_S7(t6)
    sd      s8,  REG_S8(t6)
    sd      s9,  REG_S9(t6)
    sd      s10, REG_S10(t6)
    sd      s11, REG_S11(t6)
    sd      t3,  REG_T3(t6)
    sd      t4,  REG_T4(t6)
    /* t5, t6 already saved above */

    /* Save CSRs */
    csrr    t0, sepc
    sd      t0, REG_PC(t6)
    csrr    t0, sstatus
    sd      t0, REG_SSTATUS(t6)

    /*
     * Call C handler: a0 = &thp->reg (pointer to thread's register
     * save area).  Save t6 (&thp->reg) on kernel stack for the
     * return path (t6 is caller-saved, clobbered by C code).
     */
    addi    sp, sp, -16
    sd      t6, 0(sp)
    mv      a0, t6
    call    strap_handler_c

    /*
     * C handler returned — this happens for:
     *   - Interrupt returns (timer, PLIC, IPI)
     *   - Ecall error returns
     *   - Exception recovery (xfer fault handlers)
     *
     * If the handler took the kernel_exit path (ecall → reschedule),
     * we never reach here.
     *
     * Restore from thp->reg via __kernel_restore, which sets
     * sscratch = &thp->reg before sret to U-mode.
     */
    ld      a0, 0(sp)
    addi    sp, sp, 16
    tail    __kernel_restore

/* ================================================================ */
/* S-MODE TRAP PATH                                                 */
/* Save all 31 GPRs on current kernel stack (nested frame).         */
/* tp already holds &cpupage (S-mode convention).                   */
/* ================================================================ */
.Lsmode_save:
    /*
     * t6 = 0 (old sscratch was 0, confirming S-mode).
     * sscratch = interrupted kernel code's t6.
     * sp is already a valid kernel stack.
     */
    addi    sp, sp, -FRAME_SIZE

    /* Save t5 immediately (still holds interrupted value) */
    sd      t5, REG_T5(sp)

    /* Recover interrupted t6 from sscratch, restore sscratch = 0 */
    csrrw   t5, sscratch, zero      /* t5 = interrupted t6, sscratch = 0 */
    sd      t5, REG_T6(sp)

    /* Save all other GPRs */
    sd      ra,  REG_RA(sp)
    addi    t5, sp, FRAME_SIZE      /* t5 = pre-trap sp */
    sd      t5,  REG_SP(sp)
    sd      gp,  REG_GP(sp)
    sd      tp,  REG_TP(sp)
    sd      t0,  REG_T0(sp)
    sd      t1,  REG_T1(sp)
    sd      t2,  REG_T2(sp)
    sd      s0,  REG_S0(sp)
    sd      s1,  REG_S1(sp)
    sd      a0,  REG_A0(sp)
    sd      a1,  REG_A1(sp)
    sd      a2,  REG_A2(sp)
    sd      a3,  REG_A3(sp)
    sd      a4,  REG_A4(sp)
    sd      a5,  REG_A5(sp)
    sd      a6,  REG_A6(sp)
    sd      a7,  REG_A7(sp)
    sd      s2,  REG_S2(sp)
    sd      s3,  REG_S3(sp)
    sd      s4,  REG_S4(sp)
    sd      s5,  REG_S5(sp)
    sd      s6,  REG_S6(sp)
    sd      s7,  REG_S7(sp)
    sd      s8,  REG_S8(sp)
    sd      s9,  REG_S9(sp)
    sd      s10, REG_S10(sp)
    sd      s11, REG_S11(sp)
    sd      t3,  REG_T3(sp)
    sd      t4,  REG_T4(sp)
    /* t5, t6 already saved above */

    /* Save CSRs */
    csrr    t0, sepc
    sd      t0, REG_PC(sp)
    csrr    t0, sstatus
    sd      t0, REG_SSTATUS(sp)

    /* Call C handler: a0 = pointer to stack frame */
    mv      a0, sp
    call    strap_handler_c

    /*
     * S-mode return: restore from stack frame.
     * sscratch stays 0 (S-mode convention).
     */
.Lsmode_return:
    li      t0, SSTATUS_SIE
    csrc    sstatus, t0

    ld      t0, REG_PC(sp)
    csrw    sepc, t0
    ld      t0, REG_SSTATUS(sp)
    csrw    sstatus, t0

    ld      x1,  REG_RA(sp)
    /* x2 (sp) restored last */
    ld      x3,  REG_GP(sp)
    ld      x4,  REG_TP(sp)
    ld      x5,  REG_T0(sp)
    ld      x6,  REG_T1(sp)
    ld      x7,  REG_T2(sp)
    ld      x8,  REG_S0(sp)
    ld      x9,  REG_S1(sp)
    ld      x10, REG_A0(sp)
    ld      x11, REG_A1(sp)
    ld      x12, REG_A2(sp)
    ld      x13, REG_A3(sp)
    ld      x14, REG_A4(sp)
    ld      x15, REG_A5(sp)
    ld      x16, REG_A6(sp)
    ld      x17, REG_A7(sp)
    ld      x18, REG_S2(sp)
    ld      x19, REG_S3(sp)
    ld      x20, REG_S4(sp)
    ld      x21, REG_S5(sp)
    ld      x22, REG_S6(sp)
    ld      x23, REG_S7(sp)
    ld      x24, REG_S8(sp)
    ld      x25, REG_S9(sp)
    ld      x26, REG_S10(sp)
    ld      x27, REG_S11(sp)
    ld      x28, REG_T3(sp)
    ld      x29, REG_T4(sp)
    ld      x30, REG_T5(sp)
    ld      x31, REG_T6(sp)

    /* Restore sp last (deallocate frame) */
    ld      sp, REG_SP(sp)

    sret
