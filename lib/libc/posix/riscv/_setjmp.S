/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * lib/libc/posix/riscv/_setjmp.S — _setjmp / _longjmp for RISC-V (RV64).
 *
 * Saves callee-saved integer registers (sp, s0..s11, ra) into the
 * jmp_buf's __savearea[].  No FP register save: the savearea has
 * room for only 16 8-byte slots and the integer set already takes
 * 14; the existing kernel xfer-fault path doesn't touch
 * floating point across these calls.  If FP becomes necessary,
 * bump __JMPBUFSIZE in arch/riscv/include/platform.h.
 *
 * The kernel side (arch/riscv/xfer-jmp.S) implements the
 * same layout for kernel xfer-fault recovery.  Keep them in sync.
 *
 * Layout in the savearea (offsets in bytes):
 * 0   ra
 * 8   sp
 * 16   s0
 * 24   s1
 * ...
 * 104   s11
 * (total 14 slots = 112 bytes)
 *
 * Based on FreeBSD lib/libc/riscv/gen/setjmp.S (BSD-2-Clause).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

.text

.globl _setjmp
.type _setjmp, @function
_setjmp:
    sd  ra,    0(a0)
    sd  sp,    8(a0)
    sd  s0,   16(a0)
    sd  s1,   24(a0)
    sd  s2,   32(a0)
    sd  s3,   40(a0)
    sd  s4,   48(a0)
    sd  s5,   56(a0)
    sd  s6,   64(a0)
    sd  s7,   72(a0)
    sd  s8,   80(a0)
    sd  s9,   88(a0)
    sd  s10,  96(a0)
    sd  s11, 104(a0)
    li  a0, 0
    ret
.size _setjmp, . - _setjmp

.globl _longjmp
.type _longjmp, @function
_longjmp:
    ld  ra,    0(a0)
    ld  sp,    8(a0)
    ld  s0,   16(a0)
    ld  s1,   24(a0)
    ld  s2,   32(a0)
    ld  s3,   40(a0)
    ld  s4,   48(a0)
    ld  s5,   56(a0)
    ld  s6,   64(a0)
    ld  s7,   72(a0)
    ld  s8,   80(a0)
    ld  s9,   88(a0)
    ld  s10,  96(a0)
    ld  s11, 104(a0)
    mv  a0, a1
    bnez a1, 1f
    li  a0, 1               /* longjmp must never return 0 */
1:  ret
.size _longjmp, . - _longjmp

/*
 * Mark the stack as non-executable (silences linker warnings on
 * toolchains that default to RWX).
 */
.section .note.GNU-stack,"",@progbits
