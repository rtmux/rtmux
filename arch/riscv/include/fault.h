/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RISC-V fault / exception definitions.
 *
 * Values of scause when the MSB (interrupt bit) is clear, per the RISC-V
 * Privileged Architecture specification (Table "Supervisor cause register
 * values after trap"). Reserved/unused codes are omitted.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RISCV_FAULT_H
#define _RISCV_FAULT_H

/* Asynchronous interrupt causes (scause with interrupt bit = 1, here named with
 * the interrupt bit already stripped). Supervisor-mode flavours only.
 */
#define RISCV_INT_SSW 1    /* supervisor software (IPI)      */
#define RISCV_INT_STIMER 5 /* supervisor timer               */
#define RISCV_INT_SEXT 9   /* supervisor external (PLIC)     */

/* Synchronous exception causes (scause, interrupt bit = 0). */
#define RISCV_EXC_INSN_MISALIGNED 0    /* instruction address misaligned */
#define RISCV_EXC_INSN_ACCESS_FAULT 1  /* instruction access fault       */
#define RISCV_EXC_ILLEGAL_INSN 2       /* illegal instruction            */
#define RISCV_EXC_BREAKPOINT 3         /* breakpoint (ebreak)            */
#define RISCV_EXC_LOAD_MISALIGNED 4    /* load address misaligned        */
#define RISCV_EXC_LOAD_ACCESS_FAULT 5  /* load access fault              */
#define RISCV_EXC_STORE_MISALIGNED 6   /* store/AMO address misaligned   */
#define RISCV_EXC_STORE_ACCESS_FAULT 7 /* store/AMO access fault         */
#define RISCV_EXC_ECALL_U 8            /* environment call from U-mode   */
#define RISCV_EXC_ECALL_S 9            /* environment call from S-mode   */
#define RISCV_EXC_INSN_PAGE_FAULT 12   /* instruction page fault         */
#define RISCV_EXC_LOAD_PAGE_FAULT 13   /* load page fault                */
#define RISCV_EXC_STORE_PAGE_FAULT 15  /* store/AMO page fault           */

/* Convenience predicates. */
#define RISCV_EXC_IS_PAGE_FAULT(c)                                           \
    ((c) == RISCV_EXC_INSN_PAGE_FAULT || (c) == RISCV_EXC_LOAD_PAGE_FAULT || \
     (c) == RISCV_EXC_STORE_PAGE_FAULT)

#define RISCV_EXC_IS_STORE(c)             \
    ((c) == RISCV_EXC_STORE_MISALIGNED || \
     (c) == RISCV_EXC_STORE_ACCESS_FAULT || (c) == RISCV_EXC_STORE_PAGE_FAULT)

#define RISCV_EXC_IS_ECALL(c) \
    ((c) == RISCV_EXC_ECALL_U || (c) == RISCV_EXC_ECALL_S)

#endif /* _RISCV_FAULT_H */
