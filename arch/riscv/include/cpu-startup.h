/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * \brief CPU specific definitions for startup.
 *
 * \license Apache 2.0 (http://www.apache.org/licenses/LICENSE-2.0)
 */

#ifndef _RISCV_CPU_STARTUP_H
#define _RISCV_CPU_STARTUP_H

/* Startup runs with MMU off — physical == virtual, no bias */
#define STARTUP_PADDR_BIAS 0
#define STARTUP_V2P(v) ((paddr_t)(v))

#define RISCV_MAP_SYSPAGE 0xffffffffu    /* Syspage kernel RW */
#define RISCV_MAP_SYSPAGE_RO 0xfffffffeu /* Syspage/CPUpage user R/O */
#define RISCV_MAP_USER_RX 0xfffffffdu    /* User R+X (kcall trampoline) */
#define RISCV_MAP_NOEXEC 0x80000000u     /* No execute */
#define RISCV_MAP_DEVICE 0x40000000u     /* Memory-mapped I/O */
#define RISCV_MAP_USER 0x08000000u       /* PTE_U=1: U-mode accessible */

#define CPU_SYSPAGE_TYPE SYSPAGE_RISCV

struct cpu_local_syspage {
    SYSPAGE_SECTION(riscv_cpu);
    SYSPAGE_TYPED_SECTION(paddr_t, pgdir);
};

extern uintptr_t riscv_map(uintptr_t va, paddr_t pa, size_t sz, unsigned flags);

#define CPU_COMMON_OPTIONS_STRING ""

#endif
