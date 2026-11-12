/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"

.section .text.boot, "ax"
.global _start
_start:
    msr daifset, #15
    mrs x20, CurrentEL
    lsr x20, x20, #2
    mrs x21, mpidr_el1
    and x21, x21, #0xff
    cbnz x21, .Lpark
    adrp x0, __bss_start
    add x0, x0, :lo12:__bss_start
    adrp x1, __bss_end
    add x1, x1, :lo12:__bss_end
1:  cmp x0, x1
    b.hs 2f
    str xzr, [x0], #8
    b 1b
2:  mov x22, #0
    b .Lel_setup

.global pi3_secondary_start
pi3_secondary_start:
    msr daifset, #15
    mrs x20, CurrentEL
    lsr x20, x20, #2
    mrs x21, mpidr_el1
    and x21, x21, #0xff
    cmp x21, #PI3_CPUS
    b.hs .Lpark
    mov x22, #1

/* QEMU direct boot enters EL2. Also handle firmware entry at EL3 or EL1.
 * Entry contract: MMU/caches off, little endian, all CPUs in AArch64.
 */
.Lel_setup:
    cmp x20, #3
    b.ne .Lel2
    mov x0, #0x501                 /* SCR: NS, HCE, RW */
    msr scr_el3, x0
    mov x0, #0x3c9                 /* masked, EL2h */
    msr spsr_el3, x0
    adr x0, .Lel2
    msr elr_el3, x0
    eret
.Lel2:
    mrs x0, CurrentEL
    cmp x0, #8
    b.ne .Lel1
    mov x0, #(1 << 31)             /* HCR.RW; no traps/stage 2 */
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0            /* EL1 physical counter/timer */
    msr cntvoff_el2, xzr
    msr cptr_el2, xzr
    mov x0, #0x3c5                 /* masked, EL1h */
    msr spsr_el2, x0
    adr x0, .Lel1
    msr elr_el2, x0
    eret
.Lel1:
    msr spsel, #1
    adrp x0, pi3_stacks
    add x0, x0, :lo12:pi3_stacks
    add x1, x21, #1
    add x0, x0, x1, lsl #14
    mov sp, x0
    msr cpacr_el1, xzr             /* no untracked SIMD/FP state */
    msr cntkctl_el1, xzr
    msr cntp_ctl_el0, xzr
    msr cntv_ctl_el0, xzr
    cbnz x22, 3f
    mov x0, x20
    bl pi3_primary
    b .Lpark
3:  bl pi3_secondary
.Lpark:
    wfe
    b .Lpark

.global pi3_enter_user
pi3_enter_user:
    msr elr_el1, x0
    msr sp_el0, x1
    msr spsr_el1, xzr              /* EL0t, IRQ enabled */
    mov x0, #0
    eret

/* Exact fault PCs are used by the diagnostic recovery handler. */
.global pi3_test_read, pi3_fault_read, pi3_fault_read_resume
pi3_test_read:
pi3_fault_read:
    ldr x0, [x0]
pi3_fault_read_resume:
    ret
.global pi3_test_write, pi3_fault_write, pi3_fault_write_resume
pi3_test_write:
pi3_fault_write:
    str xzr, [x0]
pi3_fault_write_resume:
    ret

.section .bss.stacks, "aw", %nobits
.balign 4096
pi3_stacks:
    .skip PI3_STACK_SIZE * PI3_CPUS
