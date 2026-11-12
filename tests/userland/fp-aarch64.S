/* SPDX-License-Identifier: Apache-2.0 */
/* Same isolation oracle as sync.c's RV64 probe, checking both lanes of every
 * AArch64 SIMD register plus FPCR/FPSR across real scheduler yields. */
.arch armv8-a+fp+simd
.text
.global fp_loop
.type fp_loop,%function
fp_loop:
    sub sp, sp, #176
    stp x19, x20, [sp]
    stp x21, x22, [sp, #16]
    stp q8, q9, [sp, #32]
    stp q10, q11, [sp, #64]
    stp q12, q13, [sp, #96]
    stp q14, q15, [sp, #128]
    mrs x9, fpcr
    mrs x10, fpsr
    stp x9, x10, [sp, #160]
    mov x19, x0
    mov x20, x1
    mov x21, #0
    lsr x22, x19, #60
    and x22, x22, #3
1:
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    add x9, x19, #\n
    dup v\n\().2d, x9
    .endr
    lsl x9, x22, #22
    msr fpcr, x9
    msr fpsr, x22
    mov x8, #90
    svc #0
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    add x9, x19, #\n
    umov x10, v\n\().d[0]
    cmp x10, x9
    cinc x21, x21, ne
    umov x10, v\n\().d[1]
    cmp x10, x9
    cinc x21, x21, ne
    .endr
    mrs x9, fpcr
    lsl x10, x22, #22
    cmp x9, x10
    cinc x21, x21, ne
    mrs x9, fpsr
    cmp x9, x22
    cinc x21, x21, ne
    subs x20, x20, #1
    b.ne 1b
    mov x0, x21
    ldp x9, x10, [sp, #160]
    msr fpcr, x9
    msr fpsr, x10
    ldp q8, q9, [sp, #32]
    ldp q10, q11, [sp, #64]
    ldp q12, q13, [sp, #96]
    ldp q14, q15, [sp, #128]
    ldp x19, x20, [sp]
    ldp x21, x22, [sp, #16]
    add sp, sp, #176
    ret
.size fp_loop, .-fp_loop

.global fp_read_all
.type fp_read_all,%function
fp_read_all:
    mrs x0, fpcr
    mrs x9, fpsr
    orr x0, x0, x9
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    umov x9, v\n\().d[0]
    orr x0, x0, x9
    umov x9, v\n\().d[1]
    orr x0, x0, x9
    .endr
    ret
.size fp_read_all, .-fp_read_all
