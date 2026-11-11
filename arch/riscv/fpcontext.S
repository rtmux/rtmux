/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* User floating-point context save/restore (RV64D).
 *
 * The kernel itself never executes an FP instruction, so a user thread's
 * f0-f31 and fcsr stay in the registers across a trap until another thread's
 * context is loaded. sstatus.FS gates FP instructions in S-mode too, so each
 * routine turns FS to Dirty for its duration and then puts back the bits it
 * found.
 *
 * Layout of the save area (RISCV_FPU_REGISTERS): f0..f31 at 0..248, fcsr at 256.
 */

    .option push
    .option arch, +d
    .text

#define SSTATUS_FS_ALL 0x6000

/* void cpu_fpu_save(RISCV_FPU_REGISTERS *a0) */
    .globl cpu_fpu_save
    .type cpu_fpu_save, @function
cpu_fpu_save:
    csrr    t1, sstatus
    li      t0, SSTATUS_FS_ALL
    csrs    sstatus, t0
    fsd     f0,    0(a0)
    fsd     f1,    8(a0)
    fsd     f2,   16(a0)
    fsd     f3,   24(a0)
    fsd     f4,   32(a0)
    fsd     f5,   40(a0)
    fsd     f6,   48(a0)
    fsd     f7,   56(a0)
    fsd     f8,   64(a0)
    fsd     f9,   72(a0)
    fsd     f10,  80(a0)
    fsd     f11,  88(a0)
    fsd     f12,  96(a0)
    fsd     f13, 104(a0)
    fsd     f14, 112(a0)
    fsd     f15, 120(a0)
    fsd     f16, 128(a0)
    fsd     f17, 136(a0)
    fsd     f18, 144(a0)
    fsd     f19, 152(a0)
    fsd     f20, 160(a0)
    fsd     f21, 168(a0)
    fsd     f22, 176(a0)
    fsd     f23, 184(a0)
    fsd     f24, 192(a0)
    fsd     f25, 200(a0)
    fsd     f26, 208(a0)
    fsd     f27, 216(a0)
    fsd     f28, 224(a0)
    fsd     f29, 232(a0)
    fsd     f30, 240(a0)
    fsd     f31, 248(a0)
    frcsr   t2
    sd      t2,  256(a0)
    and     t1, t1, t0
    csrc    sstatus, t0
    csrs    sstatus, t1
    ret
    .size cpu_fpu_save, . - cpu_fpu_save

/* void cpu_fpu_restore(const RISCV_FPU_REGISTERS *a0) */
    .globl cpu_fpu_restore
    .type cpu_fpu_restore, @function
cpu_fpu_restore:
    csrr    t1, sstatus
    li      t0, SSTATUS_FS_ALL
    csrs    sstatus, t0
    ld      t2, 256(a0)
    fscsr   t2
    fld     f0,    0(a0)
    fld     f1,    8(a0)
    fld     f2,   16(a0)
    fld     f3,   24(a0)
    fld     f4,   32(a0)
    fld     f5,   40(a0)
    fld     f6,   48(a0)
    fld     f7,   56(a0)
    fld     f8,   64(a0)
    fld     f9,   72(a0)
    fld     f10,  80(a0)
    fld     f11,  88(a0)
    fld     f12,  96(a0)
    fld     f13, 104(a0)
    fld     f14, 112(a0)
    fld     f15, 120(a0)
    fld     f16, 128(a0)
    fld     f17, 136(a0)
    fld     f18, 144(a0)
    fld     f19, 152(a0)
    fld     f20, 160(a0)
    fld     f21, 168(a0)
    fld     f22, 176(a0)
    fld     f23, 184(a0)
    fld     f24, 192(a0)
    fld     f25, 200(a0)
    fld     f26, 208(a0)
    fld     f27, 216(a0)
    fld     f28, 224(a0)
    fld     f29, 232(a0)
    fld     f30, 240(a0)
    fld     f31, 248(a0)
    and     t1, t1, t0
    csrc    sstatus, t0
    csrs    sstatus, t1
    ret
    .size cpu_fpu_restore, . - cpu_fpu_restore

    .option pop
