/* SPDX-License-Identifier: Apache-2.0 */
.section .text.boot,"ax"
.global _start
_start:
    msr daifset, #15
    mrs x21, mpidr_el1
    and x21, x21, #3
    cbnz x21, 9f
    adrp x0, _bss_start
    add x0, x0, :lo12:_bss_start
    adrp x1, _bss_end
    add x1, x1, :lo12:_bss_end
1:  cmp x0, x1
    b.hs 2f
    str xzr, [x0], #8
    b 1b
2:  mov x22, #0
    b .Lel_setup
.global _smpstart
_smpstart:
    msr daifset, #15
    mrs x21, mpidr_el1
    and x21, x21, #3
    mov x22, #1
.Lel_setup:
    mrs x0, CurrentEL
    cmp x0, #8
    b.ne 3f
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    msr cptr_el2, xzr
    mov x0, #0x3c5
    msr spsr_el2, x0
    adr x0, 3f
    msr elr_el2, x0
    eret
3:  msr spsel, #1
    adrp x0, boot_stacks
    add x0, x0, :lo12:boot_stacks
    add x1, x21, #1
    add x0, x0, x1, lsl #16
    mov sp, x0
    mov x0, #(3 << 20)
    msr cpacr_el1, x0
    mov x0, #2
    msr cntkctl_el1, x0
    msr cntv_ctl_el0, xzr
    cbnz x22, 4f
    bl aarch64_boot_mmu
    b 5f
4:  bl aarch64_secondary_mmu
5:
    ldr x0, =.Lhigh
    br x0
.Lhigh:
    ldr x0, =boot_stacks
    add x1, x21, #1
    add x0, x0, x1, lsl #16
    mov sp, x0
    cbnz x22, 6f
    bl startup_entry
6:  mov x0, x21
    bl aarch64_secondary_entry
9:  wfe
    b 9b
.section .bss,"aw",%nobits
.balign 4096
boot_stacks:
    .skip 65536 * 4
