/* SPDX-License-Identifier: BSD-2-Clause */
.text
.hidden ldso_bootstrap
.global .ldso_start
.type .ldso_start,%function
.ldso_start:
    mov x19, sp
    sub sp, sp, #16
    mov x0, x19
    mov x1, sp
    add x2, sp, #8
    bl ldso_bootstrap
    mov x16, x0
    ldr x2, [sp]
    ldr x1, [sp, #8]
    mov x0, x19
    mov sp, x19
    br x16
.size .ldso_start, .-.ldso_start
