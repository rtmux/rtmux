/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief RISC-V processor register definitions.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#ifndef __RISCV_CONTEXT_H_INCLUDED
#define __RISCV_CONTEXT_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

typedef struct riscv_cpu_registers {
    uint64_t gpr[32]; /* x0 - x31 */
    uint64_t pc;      /* Program counter */
    uint64_t sstatus; /* Saved sstatus (SPP, SPIE, etc.) */
} RISCV_CPU_REGISTERS;

/* Register names */
#define RISCV_REG_X0 0
#define RISCV_REG_X1 1
#define RISCV_REG_X2 2
#define RISCV_REG_X3 3
#define RISCV_REG_X4 4
#define RISCV_REG_X5 5
#define RISCV_REG_X6 6
#define RISCV_REG_X7 7
#define RISCV_REG_X8 8
#define RISCV_REG_X9 9
#define RISCV_REG_X10 10
#define RISCV_REG_X11 11
#define RISCV_REG_X12 12
#define RISCV_REG_X13 13
#define RISCV_REG_X14 14
#define RISCV_REG_X15 15

#define RISCV_REG_X16 16
#define RISCV_REG_X17 17
#define RISCV_REG_X18 18
#define RISCV_REG_X19 19
#define RISCV_REG_X20 20
#define RISCV_REG_X21 21
#define RISCV_REG_X22 22
#define RISCV_REG_X23 23
#define RISCV_REG_X24 24
#define RISCV_REG_X25 25
#define RISCV_REG_X26 26
#define RISCV_REG_X27 27
#define RISCV_REG_X28 28
#define RISCV_REG_X29 29
#define RISCV_REG_X30 30
#define RISCV_REG_X31 31

/* Register name aliases */
#define RISCV_REG_ZERO 0 /* Always returns 0 */
#define RISCV_REG_RA 1   /* Return address   */
#define RISCV_REG_SP 2   /* Stack pointer    */
#define RISCV_REG_GP 3   /* Global pointer   */
#define RISCV_REG_TP 4   /* Thread pointer   */
#define RISCV_REG_T0 5   /* Temporary 0      */
#define RISCV_REG_T1 6   /* Temporary 1      */
#define RISCV_REG_T2 7   /* Temporary 2      */
#define RISCV_REG_FP 8   /* Frame pointer    */
#define RISCV_REG_S1 9   /* Saved register 1 */

#define RISCV_REG_A0 10 /* Arg / retval 0   */
#define RISCV_REG_A1 11 /* Arg / retval 1   */
#define RISCV_REG_A2 12 /* Arg 2            */
#define RISCV_REG_A3 13 /* Arg 3            */
#define RISCV_REG_A4 14 /* Arg 4            */
#define RISCV_REG_A5 15 /* Arg 5            */
#define RISCV_REG_A6 16 /* Arg 6            */
#define RISCV_REG_A7 17 /* Arg 7            */

#define RISCV_REG_S2 18  /* Saved register 2 */
#define RISCV_REG_S3 19  /* Saved register 3 */
#define RISCV_REG_S4 20  /* Saved register 4 */
#define RISCV_REG_S5 21  /* Saved register 5 */
#define RISCV_REG_S6 22  /* Saved register 6 */
#define RISCV_REG_S7 23  /* Saved register 7 */
#define RISCV_REG_S8 24  /* Saved register 8 */
#define RISCV_REG_S9 25  /* Saved register 9 */
#define RISCV_REG_S10 26 /* Saved register 10 */
#define RISCV_REG_S11 27 /* Saved register 11 */

#define RISCV_REG_T3 28 /* Temporary 3      */
#define RISCV_REG_T4 29 /* Temporary 4      */
#define RISCV_REG_T5 30 /* Temporary 5      */
#define RISCV_REG_T6 31 /* Temporary 6      */

/* Register manipulation */
#define RISCV_GET_REGIP(regs) ((regs)->pc)
#define RISCV_GET_REGSP(regs) ((regs)->gpr[RISCV_REG_SP])
#define RISCV_SET_REGIP(regs, v) ((regs)->pc = v)
#define RISCV_SET_REGSP(regs, v) ((regs)->gpr[RISCV_REG_SP] = v)

typedef struct riscv_fpu_registers {
    uint64_t fpr[32];
    uint64_t fcsr; /* fflags, frm */
    uint64_t pad;  /* 272 bytes: a multiple of FPUDATA_ALIGN */
} RISCV_FPU_REGISTERS;

typedef struct riscv_alt_registers {
    /* Currently don't use any alternative registers */
    uint64_t dummy;
} RISCV_ALT_REGISTERS;

typedef struct riscv_perf_registers {
    /* Currently don't use any performance registers */
    uint64_t dummy;
} RISCV_PERF_REGISTERS;

typedef struct {
    RISCV_CPU_REGISTERS cpu;
    RISCV_FPU_REGISTERS fpu;
} mcontext_t;
#define SET_REGIP RISCV_SET_REGIP
#define SET_REGSP RISCV_SET_REGSP
#define GET_REGIP RISCV_GET_REGIP
#define GET_REGSP RISCV_GET_REGSP

#endif /* __RISCV_CONTEXT_H_INCLUDED */
