/* SPDX-License-Identifier: Apache-2.0 */
#include <exception.h>

.section .text.vectors, "ax"
.balign 2048
.global pi3_vectors
pi3_vectors:
.macro slot n
    b .Lvector\n
    .balign 128
.endm
    slot 0
    slot 1
    slot 2
    slot 3
    slot 4
    slot 5
    slot 6
    slot 7
    slot 8
    slot 9
    slot 10
    slot 11
    slot 12
    slot 13
    slot 14
    slot 15

.macro handler n
.Lvector\n:
    sub sp, sp, #A64_FRAME_SIZE
    stp x0, x1, [sp]
    mov x1, #\n
    b .Lsave
.endm
    handler 0
    handler 1
    handler 2
    handler 3
    handler 4
    handler 5
    handler 6
    handler 7
    handler 8
    handler 9
    handler 10
    handler 11
    handler 12
    handler 13
    handler 14
    handler 15
.Lsave:
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x2, spsr_el1
    mrs x3, elr_el1
    stp x3, x2, [sp, #A64_FRAME_PC]
    tst x2, #15
    b.ne 1f
    mrs x2, sp_el0
    b 2f
1:  add x2, sp, #A64_FRAME_SIZE
2:  str x2, [sp, #A64_FRAME_SP]
    mrs x2, esr_el1
    mrs x3, far_el1
    stp x2, x3, [sp, #A64_FRAME_ESR]
    mov x0, sp
    bl pi3_exception
    ldp x2, x3, [sp, #A64_FRAME_PC]
    msr elr_el1, x2
    msr spsr_el1, x3
    tst x3, #15
    b.ne 3f
    ldr x2, [sp, #A64_FRAME_SP]
    msr sp_el0, x2
3:  ldp x0, x1, [sp]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #A64_FRAME_SIZE
    eret
