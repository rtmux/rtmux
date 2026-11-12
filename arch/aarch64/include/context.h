/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_AARCH64_CONTEXT_H
#define RTMUX_AARCH64_CONTEXT_H
#include <sys/platform.h>

/* gpr[31] stores SP, not XZR. FP/SIMD is saved eagerly at every EL0 trap;
 * keeping it in the context makes migration independent of lazy FP ownership.
 */
typedef struct aarch64_cpu_registers {
    uint64_t gpr[32];
    uint64_t pc;
    uint64_t pstate;
    uint64_t tls;
    uint64_t reserved;
    uint64_t q[64];
    uint64_t fpsr;
    uint64_t fpcr;
} __attribute__((aligned(16))) AARCH64_CPU_REGISTERS;
typedef struct aarch64_fpu_registers {
    uint64_t q[64], fpsr, fpcr;
} AARCH64_FPU_REGISTERS;
typedef struct aarch64_alt_registers {
    uint64_t dummy;
} AARCH64_ALT_REGISTERS;
typedef struct aarch64_perf_registers {
    uint64_t dummy;
} AARCH64_PERF_REGISTERS;
typedef struct {
    AARCH64_CPU_REGISTERS cpu;
} mcontext_t;
#define AARCH64_REG_SP 31
#define AARCH64_REG_RA 30
#define AARCH64_REG_A0 0
#define AARCH64_REG_A7 8
#define SET_REGIP(r, v) ((r)->pc = (uintptr_t) (v))
#define SET_REGSP(r, v) ((r)->gpr[31] = (uintptr_t) (v))
#define GET_REGIP(r) ((r)->pc)
#define GET_REGSP(r) ((r)->gpr[31])
#endif
