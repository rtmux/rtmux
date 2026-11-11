/*
 * plic-regs.h — RISC-V PLIC register offsets.
 *
 * These are defined by the PLIC specification and are the same across all
 * platforms. The actual base address comes from FDT (stored in the global
 * "plic_base").
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RISCV_PLIC_REGS_H
#define _RISCV_PLIC_REGS_H

/* Priority registers: one 32-bit word per source (source 0 is reserved) */
#define PLIC_OFF_PRIORITY 0x0

/* Pending bits: 1 bit per source, packed into 32-bit words */
#define PLIC_OFF_PENDING 0x1000

/* The PLIC's per-context registers are indexed by CONTEXT number, not by hart.
 * A "context" is a (hart, privilege-mode) pair in the order listed in the
 * PLIC's FDT "interrupts-extended" property.
 *
 * On QEMU virt, every hart has (M, S) contexts, so hart N's S-mode context is
 * (2*N + 1). On SiFive FU540/FU740 hart 0 is the E-mode monitor core with only
 * an M-mode context, which shifts all subsequent S-mode contexts down by one.
 * See board.c for the plic_s_ctx[] table computed from the actual FDT cpu@N
 * layout.
 */
extern unsigned plic_s_ctx[];

#define PLIC_OFF_SENABLE(hart) (0x002000 + plic_s_ctx[hart] * 0x80)
#define PLIC_OFF_SPRIORITY(hart) (0x200000 + plic_s_ctx[hart] * 0x1000)
#define PLIC_OFF_SCLAIM(hart) (0x200004 + plic_s_ctx[hart] * 0x1000)

#endif /* _RISCV_PLIC_REGS_H */
