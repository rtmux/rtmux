/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_AARCH64_SYSPAGE_H
#define RTMUX_AARCH64_SYSPAGE_H
#define SYSPAGE_KERNEL_ENTRY
#define ARCH_SYSPAGE_ENTRY struct aarch64_syspage_entry aarch64
struct aarch64_cpu_entry {
    uint64_t (*clock_cycles)(void);
};
struct aarch64_syspage_entry {
    uint64_t startup_base;
    unsigned startup_size;
    syspage_entry_info cpu;
};
#endif
