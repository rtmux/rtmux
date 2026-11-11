/*
 * xfer-jmp.S — _setjmp / _longjmp for kernel xfer fault recovery (RISC-V)
 *
 * Saves/restores callee-saved registers only (no signal mask, no FP).
 * Based on FreeBSD lib/libc/riscv/gen/setjmp.S (BSD-2-Clause).
 *
 * License: BSD-2-Clause
 */

.text

.globl _setjmp
.type _setjmp, @function
_setjmp:
    sd  ra,   0(a0)
    sd  sp,   8(a0)
    sd  s0,  16(a0)
    sd  s1,  24(a0)
    sd  s2,  32(a0)
    sd  s3,  40(a0)
    sd  s4,  48(a0)
    sd  s5,  56(a0)
    sd  s6,  64(a0)
    sd  s7,  72(a0)
    sd  s8,  80(a0)
    sd  s9,  88(a0)
    sd  s10, 96(a0)
    sd  s11, 104(a0)
    li  a0, 0
    ret
.size _setjmp, . - _setjmp

.globl _longjmp
.type _longjmp, @function
_longjmp:
    ld  ra,   0(a0)
    ld  sp,   8(a0)
    ld  s0,  16(a0)
    ld  s1,  24(a0)
    ld  s2,  32(a0)
    ld  s3,  40(a0)
    ld  s4,  48(a0)
    ld  s5,  56(a0)
    ld  s6,  64(a0)
    ld  s7,  72(a0)
    ld  s8,  80(a0)
    ld  s9,  88(a0)
    ld  s10, 96(a0)
    ld  s11, 104(a0)
    mv  a0, a1
    bnez a1, 1f
    li  a0, 1           /* never return 0 from longjmp */
1:  ret
.size _longjmp, . - _longjmp
