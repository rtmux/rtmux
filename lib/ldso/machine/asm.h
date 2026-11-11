/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Minimal machine/asm.h for RTMux ldso (RISC-V) */
#ifndef _MACHINE_ASM_H
#define _MACHINE_ASM_H

#define ENTRY(sym)        \
    .text;                \
    .globl sym;           \
    .type sym, @function; \
    .align 4;             \
    sym:                  \
    .cfi_startproc;
#define END(sym)  \
    .cfi_endproc; \
    .size sym, .- sym

#define EENTRY(sym) \
    .globl sym;     \
    sym:
#define EEND(sym)

#endif
