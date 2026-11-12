/* SPDX-License-Identifier: Apache-2.0 */
#include "pi3.h"

#define TABLE 3UL
#define BLOCK 1UL
#define AF BIT64(10)
#define INNER_SHAREABLE (3UL << 8)
#define NORMAL (1UL << 2)
#define RO BIT64(7)
#define USER BIT64(6)
#define PXN BIT64(53)
#define UXN BIT64(54)
#define RAM (AF | INNER_SHAREABLE | NORMAL | UXN)
#define DEVICE (AF | PXN | UXN)
#define PAGE_SIZE 4096U

/* 39-bit VA, three levels, 4 KiB pages. TTBR1 mirrors RAM/device mappings at
 * RTMux's existing direct-map base, but bootstrap code executes at low VA.
 * First 2 MiB and the EL0 fixture use pages; other RAM uses 2 MiB blocks.
 */
static uint64_t lower[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t upper[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t ram[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t devices[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t boot[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t user[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t probe_a[512] __attribute__((aligned(PAGE_SIZE)));
static uint64_t probe_b[512] __attribute__((aligned(PAGE_SIZE)));

void pi3_mmu_init(void)
{
    lower[0] = (uintptr_t) ram | TABLE;
    lower[1] = (uintptr_t) devices | TABLE;
    /* APTable[0] and UXNTable prohibit EL0 through the direct-map alias,
     * including aliases of pages that are user-accessible through TTBR0. */
    upper[256] = (uintptr_t) ram | TABLE | BIT64(61) | BIT64(60);
    upper[257] = (uintptr_t) devices | TABLE | BIT64(61) | BIT64(60);

    for (unsigned i = 0; i < PI3_RAM_END / 0x200000; i++)
        ram[i] = (uint64_t) i * 0x200000 | RAM | PXN | BLOCK;
    /* GPU-reserved RAM is deliberately left unmapped. */
    for (unsigned i = 0x3f000000 / 0x200000; i < 512; i++)
        ram[i] = (uint64_t) i * 0x200000 | DEVICE | BLOCK;
    devices[0] = (uint64_t) PI3_LOCAL | DEVICE | BLOCK;
    ram[0] = (uintptr_t) boot | TABLE;
    ram[1] = (uintptr_t) user | TABLE;

    for (unsigned i = 0; i < 512; i++) {
        uintptr_t pa = (uintptr_t) i * PAGE_SIZE;
        uint64_t attributes = RAM | PXN;
        if (pa >= (uintptr_t) __text_start && pa < (uintptr_t) __text_end)
            attributes = RAM | RO;
        if (pa >= (uintptr_t) __rodata_start && pa < (uintptr_t) __rodata_end)
            attributes |= RO;
        boot[i] = pa | attributes | TABLE;
    }
    /* EL0 may execute only its own text; all writable memory is XN. */
    user[0] =
        PI3_USER_TEXT | AF | INNER_SHAREABLE | NORMAL | USER | RO | PXN | TABLE;
    user[1] = PI3_USER_DATA | RAM | USER | PXN | TABLE;
    user[4] = PI3_USER_STACK | RAM | USER | PXN | TABLE;
}

void pi3_mmu_enable(void)
{
    uint64_t sctlr;
    /* MAIR index 0: Device-nGnRnE. Index 1: Normal WB RA/WA cacheable. */
    uint64_t mair = 0xff00UL;
    /* T0SZ/T1SZ=25, inner-shareable WB walks, TG0/TG1=4K, IPS=32 bit. */
    uint64_t tcr = 25 | (1UL << 8) | (1UL << 10) | (3UL << 12) | (25UL << 16) |
                   (1UL << 24) | (1UL << 26) | (3UL << 28) | (2UL << 30);
    __asm__ volatile(
        "dsb sy; msr mair_el1, %0; msr tcr_el1, %1;"
        "msr ttbr0_el1, %2; msr ttbr1_el1, %3; isb;"
        "tlbi vmalle1; dsb sy; isb; ic iallu; dsb sy; isb" ::"r"(mair),
        "r"(tcr), "r"(lower), "r"(upper)
        : "memory");
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    /* RES1 bits for Armv8.0, WXN, I, SA0, SA, C and M. Keep data unaligned
     * accesses legal: RTMux's byte-stream parsers use them. */
    sctlr |= 0x30d00800UL | BIT64(19) | BIT64(12) | BIT64(4) | BIT64(3) |
             BIT64(2) | BIT64(0);
    sctlr &= ~(BIT64(25) | BIT64(24) | BIT64(1));
    __asm__ volatile("msr sctlr_el1, %0; isb" ::"r"(sctlr) : "memory");
}

void pi3_mmu_probe_map(uintptr_t physical)
{
    unsigned index = (PI3_PROBE_VA - PI3_USER_TEXT) / PAGE_SIZE;
    /* Break before make, including global mappings and other CPUs. */
    user[index] = 0;
    __asm__ volatile(
        "dsb ishst; tlbi vaae1is, %0; tlbi vaae1is, %1; dsb ish; isb" ::"r"(
            (uintptr_t) PI3_PROBE_VA >> 12),
        "r"(((PI3_HIGH_BASE + PI3_PROBE_VA) >> 12) & ((1UL << 44) - 1))
        : "memory");
    user[index] = physical | RAM | PXN | TABLE;
    __asm__ volatile("dsb ishst; isb" ::: "memory");
}

void pi3_mmu_test(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if ((sctlr & (BIT64(12) | BIT64(2) | BIT64(0))) !=
        (BIT64(12) | BIT64(2) | BIT64(0)))
        pi3_fail("mmu-disabled");
    volatile uint64_t *alias = (void *) (PI3_HIGH_BASE + (uintptr_t) probe_a);
    probe_a[0] = 0x123456789abcdef0UL;
    if (*alias != probe_a[0])
        pi3_fail("mmu-high-alias");
    *alias = 0xa53;
    if (probe_a[0] != 0xa53)
        pi3_fail("mmu-alias-write");
    pi3_pass("mmu-cache-high-alias");

    probe_b[0] = 0xb64;
    volatile uint64_t *probe = (void *) (uintptr_t) PI3_PROBE_VA;
    pi3_mmu_probe_map((uintptr_t) probe_a);
    if (*probe != 0xa53)
        pi3_fail("mmu-first-mapping");
    pi3_mmu_probe_map((uintptr_t) probe_b);
    if (*probe != 0xb64)
        pi3_fail("mmu-tlb-remap");
    pi3_pass("mmu-tlb-remap");

    pi3_expect_fault(0x25, 6, 0x50000000, (uintptr_t) pi3_fault_read,
                     (uintptr_t) pi3_fault_read_resume, false);
    (void) pi3_test_read(0x50000000);
    pi3_fault_consumed();
    pi3_pass("el1-unmapped-fault");
    pi3_expect_fault(0x25, 15, (uintptr_t) __text_start,
                     (uintptr_t) pi3_fault_write,
                     (uintptr_t) pi3_fault_write_resume, true);
    pi3_test_write((uintptr_t) __text_start);
    pi3_fault_consumed();
    pi3_pass("el1-readonly-fault");
}
