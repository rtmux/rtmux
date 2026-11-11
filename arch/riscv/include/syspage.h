/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _RISCV_SYSPAGE_H
#define _RISCV_SYSPAGE_H

/* kcall moved to dedicated trampoline page — syspage is now pure R/O */
#define SYSPAGE_KERNEL_ENTRY /* removed */
#define ARCH_SYSPAGE_ENTRY struct riscv_syspage_entry riscv

struct riscv_cpu_entry {
    uint64_t (*clock_cycles)(void);
};

struct riscv_syspage_entry {
    uint64_t startup_base;
    unsigned startup_size;
    syspage_entry_info cpu;
};

/* riscv_kernel_entry removed — ecall trampoline is now a separate page */

#endif
