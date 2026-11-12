/* SPDX-License-Identifier: Apache-2.0 */
.arch armv8-a+fp+simd
.text
.balign 2048
.global aarch64_vectors
aarch64_vectors:
    b .Ltrap_sync
    .balign 128
    b .Ltrap_irq
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_irq
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_irq
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_irq
    .balign 128
    b .Ltrap_sync
    .balign 128
    b .Ltrap_sync
    .balign 128

.macro save_context
    sub sp, sp, #816
    stp x0, x1, [sp, #0]
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
    mrs x9, spsr_el1
    str x9, [sp, #264]
    tst x9, #15
    b.ne 1f
    mrs x9, sp_el0
    b 2f
1:  add x9, sp, #816
2:  str x9, [sp, #248]
    mrs x9, elr_el1
    str x9, [sp, #256]
    mrs x9, tpidr_el0
    str x9, [sp, #272]
    add x9, sp, #288
    stp q0, q1, [x9, #0]
    stp q2, q3, [x9, #32]
    stp q4, q5, [x9, #64]
    stp q6, q7, [x9, #96]
    stp q8, q9, [x9, #128]
    stp q10, q11, [x9, #160]
    stp q12, q13, [x9, #192]
    stp q14, q15, [x9, #224]
    stp q16, q17, [x9, #256]
    stp q18, q19, [x9, #288]
    stp q20, q21, [x9, #320]
    stp q22, q23, [x9, #352]
    stp q24, q25, [x9, #384]
    stp q26, q27, [x9, #416]
    stp q28, q29, [x9, #448]
    stp q30, q31, [x9, #480]
    mrs x9, fpsr
    str x9, [sp, #800]
    mrs x9, fpcr
    str x9, [sp, #808]
.endm
.Ltrap_sync:
    save_context
    mov x1, #0
    b .Ldispatch
.Ltrap_irq:
    save_context
    mov x1, #1
.Ldispatch:
    mov x0, sp
    bl aarch64_exception
    mov x0, sp
    add x1, sp, #816
    b aarch64_restore
.global aarch64_restore
.type aarch64_restore,%function
aarch64_restore:
    msr daifset, #15
    clrex
    mov x30, x0
    ldr x9, [x30, #256]
    msr elr_el1, x9
    ldr x9, [x30, #264]
    msr spsr_el1, x9
    ldr x10, [x30, #248]
    tst x9, #15
    b.ne 1f
    msr sp_el0, x10
    mov sp, x1
    b 2f
1:  mov sp, x10
2:  ldr x9, [x30, #272]
    msr tpidr_el0, x9
    ldr x9, [x30, #800]
    msr fpsr, x9
    ldr x9, [x30, #808]
    msr fpcr, x9
    add x9, x30, #288
    ldp q0, q1, [x9, #0]
    ldp q2, q3, [x9, #32]
    ldp q4, q5, [x9, #64]
    ldp q6, q7, [x9, #96]
    ldp q8, q9, [x9, #128]
    ldp q10, q11, [x9, #160]
    ldp q12, q13, [x9, #192]
    ldp q14, q15, [x9, #224]
    ldp q16, q17, [x9, #256]
    ldp q18, q19, [x9, #288]
    ldp q20, q21, [x9, #320]
    ldp q22, q23, [x9, #352]
    ldp q24, q25, [x9, #384]
    ldp q26, q27, [x9, #416]
    ldp q28, q29, [x9, #448]
    ldp q30, q31, [x9, #480]
    ldp x0, x1, [x30, #0]
    ldp x2, x3, [x30, #16]
    ldp x4, x5, [x30, #32]
    ldp x6, x7, [x30, #48]
    ldp x8, x9, [x30, #64]
    ldp x10, x11, [x30, #80]
    ldp x12, x13, [x30, #96]
    ldp x14, x15, [x30, #112]
    ldp x16, x17, [x30, #128]
    ldp x18, x19, [x30, #144]
    ldp x20, x21, [x30, #160]
    ldp x22, x23, [x30, #176]
    ldp x24, x25, [x30, #192]
    ldp x26, x27, [x30, #208]
    ldp x28, x29, [x30, #224]
    ldr x30, [x30, #240]
    eret
.global aarch64_idle_enter
aarch64_idle_enter:
    mov sp, x1
    b cpu_idle
.global kernel_thread_checkpoint
kernel_thread_checkpoint:
    stp x19, x20, [x0, #152]
    stp x21, x22, [x0, #168]
    stp x23, x24, [x0, #184]
    stp x25, x26, [x0, #200]
    stp x27, x28, [x0, #216]
    stp x29, x30, [x0, #232]
    mov x9, sp
    str x9, [x0, #248]
    str x30, [x0, #256]
    mov x9, #5
    str x9, [x0, #264]
    mov x9, #1
    str x9, [x0]
    mov x0, #0
    ret

.global ksys_start
ksys_start:
    mrs x0,tpidr_el1
    ldr w0,[x0,#8]
    adrp x1,ksys_stack
    add x1,x1,:lo12:ksys_stack
    ldr x1,[x1,x0,lsl #3]
    mov sp,x1
    b kernel_exit
