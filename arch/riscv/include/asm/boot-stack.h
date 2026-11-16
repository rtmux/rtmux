/* SPDX-License-Identifier: Apache-2.0 */

#ifndef _RISCV_ASM_BOOT_STACK_H
#define _RISCV_ASM_BOOT_STACK_H

/* The startup stack: BOOT_STACK_SIZE bytes from _end up, growing down from
 * _end + BOOT_STACK_SIZE (head.S). head.S fills it with BOOT_STACK_FILL before
 * the first call, so boot_layout_check() can tell how deep the startup path
 * went. The boot workspace starts above it (kernel/startup/ram.c).
 *
 * Preprocessor only: head.S includes this too.
 */
#define BOOT_STACK_SIZE 0x4000
#define BOOT_STACK_FILL 0x5a5a5a5a5a5a5a5a

/* Bytes at the low end that must stay untouched: a deeper stack has overflowed */
#define BOOT_STACK_GUARD 256

#endif
