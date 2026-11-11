/*
 * RISC-V kernel entry/exit assembly.
 *
 * ksys_start    — called once per CPU from kernel_init() to begin running
 *                the idle thread.
 * __kernel_restore — restore full register context from RISCV_CPU_REGISTERS
 *                 and return to thread via sret.
 * License: Apache 2.0
 */

#include <trap-asm.h>
#include <arch/pcpu.h>          /* PC_HART, PC_CURTHREAD — asm-visible */

    .text
    .option norvc           /* no compressed instructions in kernel asm */

    .globl ksys_start
    .globl __kernel_restore

/*
 * ksys_start — start execution of the first thread on this CPU.
 *
 * Called from kernel_init() after all initialization is done.
 * tp already holds &cpupage (set by kmain/kernel_init).
 * Switches to the kernel stack and jumps to kernel_exit (C function),
 * which restores the current thread's register frame and srets to the
 * idle thread.
 *
 * The boot CPU (any hart) enters here once after kernel_init().
 */
ksys_start:
    /*
     * Switch to this CPU's kernel stack: ksys_stack[get_cpunum()].
     * tp = &pcpu_array[hart], so the hart ID is one lw at PC_HART(tp)
     * — no chain.
     */
    lwu  t1, PC_HART(tp)        /* t1 = pc_hart */
    slli t1, t1, 3              /* t1 = hart * 8 */
    la   t0, ksys_stack
    add  t0, t0, t1             /* t0 = &ksys_stack[hart] */
    ld   sp, 0(t0)

    /* Ensure sscratch = 0 (S-mode convention) */
    csrw sscratch, zero

    /* Jump to kernel_exit (C function, never returns) */
    tail kernel_exit


/*
 * __kernel_restore — restore all GPRs from a RISCV_CPU_REGISTERS struct
 *                 and return to the thread via sret.
 *
 * Called from kernel_exit() as the final step of leaving the kernel.
 * Also used as the return path from U-mode trap handlers (tail call).
 * This function never returns.
 *
 * Arguments:
 *   a0 = pointer to RISCV_CPU_REGISTERS (in kernel memory):
 *         gpr[0..31] at offsets REG_ZERO..REG_T6
 *         pc         at offset REG_PC
 *         sstatus    at offset REG_SSTATUS
 *
 * sscratch protocol:
 *   If returning to U-mode (SPP=0): set sscratch = a0 (&thp->reg)
 *   If returning to S-mode (SPP=1): leave sscratch = 0
 */
__kernel_restore:
    /*
     * Disable interrupts FIRST — before loading from the save area.
     * A timer interrupt during register restoration could corrupt
     * state if a nested kernel_exit changes actives[].
     */
    li   t0, SSTATUS_SIE
    csrc sstatus, t0

    /* Validate that a0 points to kernel space */
    li   t0, 0xffffffc000000000
    bgeu a0, t0, 1f
    /* a0 is not a kernel pointer — crash */
    li   t0, SSTATUS_SIE
    csrs sstatus, t0                    /* re-enable for kprintf */
    call __kernel_restore_bad_ptr
1:
    /* Load saved PC into sepc — catch NULL before sret */
    ld   t0, REG_PC(a0)
    bnez t0, 2f
    /* PC is NULL — crash with diagnostic */
    li   t0, SSTATUS_SIE
    csrs sstatus, t0                    /* re-enable for kprintf */
    call __kernel_restore_null_pc
2:
    csrw sepc, t0

    /*
     * Load saved sstatus.  Check SPP to decide sscratch:
     *   SPP=0 (U-mode return): validate PC is U-mode, arm sscratch
     *   SPP=1 (S-mode return): validate sp/tp are kernel-valid
     *
     * Before the SPP test, sanity-check sstatus itself.  Bits 39-62
     * are RES0 in sstatus — they should never be set in any valid
     * value.  When sstatus has been overwritten by a stray kernel
     * pointer, bits 39+ are set.  Reject those values.
     *
     * NOTE: do NOT require bits 32-33 (UXL) to equal 0b10 — RTMux's
     * thread-init paths in cpu-misc.c and the S-mode ECALL fixup
     * in trap-handler.c write sstatus without UXL on purpose
     * (e.g. 0x40120 = SPP|SPIE|SUM); the hardware csrw preserves
     * UXL because it is WARL.  Tightening the check to require
     * UXL=2 would false-positive every kernel-thread dispatch.
     */
    ld   t0, REG_SSTATUS(a0)
    li   t2, 0x7fffff8000000000UL       /* mask bits 39-62 */
    and  t1, t0, t2
    bnez t1, .Lker_restore_bad_sstatus

    csrw sstatus, t0
    andi t1, t0, SSTATUS_SPP
    beqz t1, .Lker_restore_u_return

    /*
     * Catch the "S-mode return with broken sp/tp" corruption class.
     * When sret dispatches an S-mode thread
     * (roottask, kernel-side worker) with reg.sp=0 or reg.tp=0, the
     * thread runs but the very first store to its kernel stack or
     * deref of its TLS page-faults at a tiny address; the kernel
     * mis-classifies that fault and the post-mortem points at
     * whatever routine landed on the bad context, not at the
     * dispatch.
     *
     * Validate here, BEFORE sret:
     *   reg.sp must lie in [KERN_SP_LO .. KERN_SP_HI)
     *   reg.tp must be non-zero
     *
     * KERN_SP_LO/HI are 0xffffffc080000000 / 0xffffffc100000000
     * (the direct-map RAM VA range) — same constants chk_kernel_sp
     * uses in cpu-inline.h.
     */
    ld   t1, REG_SP(a0)
    li   t2, 0xffffffc080000000UL       /* KERN_SP_LO */
    bltu t1, t2, .Lker_restore_bad_s_ctx
    li   t2, 0xffffffc100000000UL       /* KERN_SP_HI */
    bgeu t1, t2, .Lker_restore_bad_s_ctx
    ld   t1, REG_TP(a0)
    beqz t1, .Lker_restore_bad_s_ctx
    j    3f

.Lker_restore_bad_s_ctx:
    li   t1, SSTATUS_SIE
    csrs sstatus, t1                    /* re-enable for kprintf */
    call __kernel_restore_bad_smode_ctx

.Lker_restore_bad_sstatus:
    li   t1, SSTATUS_SIE
    csrs sstatus, t1                    /* re-enable for kprintf */
    call __kernel_restore_bad_sstatus

.Lker_restore_u_return:
    /*
     * Catch the "U-mode return with kernel PC" corruption class.
     * If SPP=0 (returning to U-mode) but the
     * saved PC lies in the kernel half of the address space, sret
     * would put a U-mode thread executing kernel addresses.
     * RISC-V hardware lets that happen as long as the kernel page
     * has PTE_U=1 (which kernel text does); the resulting fault is
     * then misclassified as a U-mode page fault even though the
     * trapping PC is a kernel address — the !UFLT! fingerprint.
     *
     * Crash with context BEFORE sret so the post-mortem points at
     * the dispatched thread and the corrupted reg.pc, not at
     * whatever kernel routine the U-mode thread happens to land
     * on.
     */
    ld   t1, REG_PC(a0)                 /* reload saved PC */
    li   t2, 0xffffffc000000000UL
    bltu t1, t2, 4f                     /* PC < KERNEL_VIRT_BASE → ok */
    /* PC is in kernel half on U-mode return — crash */
    li   t1, SSTATUS_SIE
    csrs sstatus, t1                    /* re-enable for kprintf */
    call __kernel_restore_kernel_pc_on_u_return
4:
    /* U-mode return — arm sscratch for next trap entry */
    csrw sscratch, a0
3:
    /*
     * Restore all GPRs except x0 (zero) and x10 (a0).
     * a0 is restored last since it's the base pointer.
     */
    ld   x1,  REG_RA(a0)               /* ra  */
    ld   x2,  REG_SP(a0)               /* sp  */
    ld   x3,  REG_GP(a0)               /* gp  */
    ld   x4,  REG_TP(a0)               /* tp  */
    ld   x5,  REG_T0(a0)               /* t0  */
    ld   x6,  REG_T1(a0)               /* t1  */
    ld   x7,  REG_T2(a0)               /* t2  */
    ld   x8,  REG_S0(a0)               /* s0  */
    ld   x9,  REG_S1(a0)               /* s1  */
    /* x10 (a0) skipped — restored last below */
    ld   x11, REG_A1(a0)               /* a1  */
    ld   x12, REG_A2(a0)               /* a2  */
    ld   x13, REG_A3(a0)               /* a3  */
    ld   x14, REG_A4(a0)               /* a4  */
    ld   x15, REG_A5(a0)               /* a5  */
    ld   x16, REG_A6(a0)               /* a6  */
    ld   x17, REG_A7(a0)               /* a7  */
    ld   x18, REG_S2(a0)               /* s2  */
    ld   x19, REG_S3(a0)               /* s3  */
    ld   x20, REG_S4(a0)               /* s4  */
    ld   x21, REG_S5(a0)               /* s5  */
    ld   x22, REG_S6(a0)               /* s6  */
    ld   x23, REG_S7(a0)               /* s7  */
    ld   x24, REG_S8(a0)               /* s8  */
    ld   x25, REG_S9(a0)               /* s9  */
    ld   x26, REG_S10(a0)              /* s10 */
    ld   x27, REG_S11(a0)              /* s11 */
    ld   x28, REG_T3(a0)               /* t3  */
    ld   x29, REG_T4(a0)               /* t4  */
    ld   x30, REG_T5(a0)               /* t5  */
    ld   x31, REG_T6(a0)               /* t6  */

    /* Restore a0 last — a0 still holds the struct base address */
    ld   a0, REG_A0(a0)

    sret
