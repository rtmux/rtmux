/* SPDX-License-Identifier: Apache-2.0 */
.section .rtmux, "a"
.balign 4
.long 0x4d555800
.short 1, 1
.long 0, 1
.zero 16
.text
.global _start
.type _start, %function
_start:
    mov x29, xzr
    mov sp, x0
    ldr x19, [sp]
    add x20, sp, #8
    add x21, x20, x19, lsl #3
    add x21, x21, #8
    adrp x9, :got:environ
    ldr x9, [x9, :got_lo12:environ]
    str x21, [x9]
    bl __rtmux_syschan_init
    mov x0, x19
    mov x1, x20
    mov x2, x21
    bl main
    bl exit
    brk #0
.size _start, .-_start
.bss
.balign 8
.weak environ
.type environ, %object
.size environ, 8
environ: .zero 8
