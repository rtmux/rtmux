/* SPDX-License-Identifier: Apache-2.0 */
.text
.global pi3_simd_probe
.type pi3_simd_probe,%function
pi3_simd_probe:
    stp x19, x30, [sp, #-32]!
    str q8, [sp, #16]
    mov x8, x1
    mov x19, x0
    dup v0.2d, x19
    dup v8.2d, x19
    dup v31.2d, x19
    // Timer interrupts can arrive while SIMD registers contain live values.
    mov x1, #8192
1:  subs x1, x1, #1
    b.ne 1b
    svc #0
    umov x1, v0.d[0]
    cmp x1, x19
    b.ne 2f
    umov x1, v0.d[1]
    cmp x1, x19
    b.ne 2f
    umov x1, v8.d[1]
    cmp x1, x19
    b.ne 2f
    umov x1, v31.d[1]
    cmp x1, x19
    b.ne 2f
    mov x0, #0
    b 3f
2:  mov x0, #1
3:  ldr q8, [sp, #16]
    ldp x19, x30, [sp], #32
    ret
.size pi3_simd_probe,.-pi3_simd_probe
.section .note.GNU-stack,"",%progbits
