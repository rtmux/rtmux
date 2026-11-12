/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"

/* This fixture uses a private bootstrap SVC protocol, not the RTMux libc ABI. */
.section .text, "ax"
.global _start
_start:
.macro seed n
    mov x\n, #\n
.endm
.irp n,2,3,4,5,6,7,10,11,12,13,14,15,16,17,18,20,21,22,23,24,25,26,27,28,29,30
    seed \n
.endr
    mov x19, #0x1919
    mov x9, #0x9999
    mov x0, #19
    mov x1, #23
    mov x8, #0
    cmp x0, x0                     /* NZCV=0110 */
    svc #PI3_TEST_SVC
    b.ne .Lbad
    b.cc .Lbad
    cmp x0, #42
    b.ne .Lbad
    cmp x1, #23
    b.ne .Lbad
.macro check n
    cmp x\n, #\n
    b.ne .Lbad
.endm
.irp n,2,3,4,5,6,7,10,11,12,13,14,15,16,17,18,20,21,22,23,24,25,26,27,28,29,30
    check \n
.endr
    cbnz x8, .Lbad
    mov x10, #0x1919
    cmp x19, x10
    b.ne .Lbad
    mov x10, #0x9999
    cmp x9, x10
    b.ne .Lbad
    mov x8, #99
    svc #PI3_TEST_SVC
    cmn x0, #1
    b.ne .Lbad

    /* EL0 cannot read privileged kernel text. */
    mov x0, #0x80000
    adr x1, 1f
    mov x8, #2
    svc #PI3_TEST_SVC
    ldr x2, [x0]
1:
    /* Nor write its own read-only text. */
    mov x0, #PI3_USER_TEXT
    adr x1, 2f
    mov x8, #2
    svc #PI3_TEST_SVC
    str xzr, [x0]
2:
    /* Nor execute writable data (W^X). */
    ldr x0, =PI3_USER_DATA
    adr x1, 3f
    mov x8, #2
    svc #PI3_TEST_SVC
    blr x0
3:
    /* TTBR1's direct-map alias must not bypass EL0 restrictions. */
    ldr x0, =(PI3_HIGH_BASE + PI3_USER_DATA)
    adr x1, 4f
    mov x8, #2
    svc #PI3_TEST_SVC
    ldr x2, [x0]
4:
    /* Guard page immediately below the user stack remains unmapped. */
    ldr x0, =(PI3_USER_STACK - 4096)
    adr x1, 5f
    mov x8, #2
    svc #PI3_TEST_SVC
    ldr x2, [x0]
5:
    /* Writable user data must still be usable. */
    ldr x0, =PI3_USER_DATA
    mov x2, #0xa53
    str x2, [x0]
    ldr x3, [x0]
    cmp x2, x3
    b.ne .Lbad
    /* Timer IRQs must enter through the lower-EL vector and restore EL0. */
    mov x8, #3
    svc #PI3_TEST_SVC
    mov x10, #0x1010
    ldr x11, =PI3_USER_DATA
6:  ldr x12, [x11]
    cmp x12, #5
    b.hs 7f
    yield
    b 6b
7:  mov x0, #0x1010
    cmp x10, x0
    b.ne .Lbad
    mov x0, #0x1919
    cmp x19, x0
    b.ne .Lbad
    mov x8, #4
    svc #PI3_TEST_SVC
    mov x0, #0
    mov x8, #1
    svc #PI3_TEST_SVC
.Lbad:
    mov x0, #1
    mov x8, #1
    svc #PI3_TEST_SVC
    b .Lbad
    .ltorg

.section .data, "aw"
    .quad 0

/* RTMux identity layout from sys/rtmux-ident.h. ABI 0: test fixture only. */
.section .rtmux, "a"
    .long 0x4d555800
    .short 1, 1
    .long 1, 0
    .zero 16
