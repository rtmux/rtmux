/* SPDX-License-Identifier: Apache-2.0 */
#include <stdint.h>
/* Called at the load PA: ADRP references below resolve to physical addresses.
 */
uint64_t boot_lower[512] __attribute__((aligned(4096)));
uint64_t boot_upper[512] __attribute__((aligned(4096)));
static uint64_t boot_ram[512] __attribute__((aligned(4096)));
static uint64_t boot_local[512] __attribute__((aligned(4096)));
void aarch64_boot_mmu(void)
{
    boot_lower[0] = (uintptr_t) boot_ram | 3;
    boot_upper[256] = (uintptr_t) boot_ram | 3 | (1UL << 61) | (1UL << 60);
    boot_upper[257] = (uintptr_t) boot_local | 3 | (1UL << 61) | (1UL << 60);
    for (unsigned i = 0; i < 480; i++)
        boot_ram[i] = (uint64_t) i * 0x200000 | 0x705 | (1UL << 54);
    for (unsigned i = 504; i < 512; i++)
        boot_ram[i] = (uint64_t) i * 0x200000 | 0x401 | (3UL << 53);
    boot_local[0] = 0x40000401UL | (3UL << 53);
    uint64_t tcr = 25 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (25UL << 16) |
                   (1UL << 24) | (1UL << 26) | (3UL << 28) | (2UL << 30);
    __asm__ volatile(
        "dsb sy; msr mair_el1,%0; msr tcr_el1,%1; msr ttbr0_el1,%2; msr "
        "ttbr1_el1,%3; isb; tlbi vmalle1; dsb sy; isb" ::"r"(0xff00UL),
        "r"(tcr), "r"(boot_lower), "r"(boot_upper)
        : "memory");
    __asm__ volatile("msr sctlr_el1,%0; isb" ::"r"(0x30d0181dUL) : "memory");
}

uint64_t aarch64_kernel_ttbr1;
void aarch64_secondary_mmu(void)
{
    uint64_t tcr = 25 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (25UL << 16) |
                   (1UL << 24) | (1UL << 26) | (3UL << 28) | (2UL << 30);
    __asm__ volatile(
        "dsb sy; msr mair_el1,%0; msr tcr_el1,%1; msr ttbr0_el1,%2; msr "
        "ttbr1_el1,%3; isb; tlbi vmalle1; dsb sy; isb; msr sctlr_el1,%4; isb" ::
            "r"(0xff00UL),
        "r"(tcr), "r"(boot_lower), "r"(aarch64_kernel_ttbr1), "r"(0x30d0181dUL)
        : "memory");
}
