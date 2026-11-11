/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* RISC-V VM internals for RTMux.
 *
 * Sv39: 3-level page table, 39-bit virtual address space
 *   Levels: VPN[2] (root) -> VPN[1] -> VPN[0] -> page offset
 *   Each level: 512 entries x 8 bytes = 4KB page table
 */

#ifndef CPU_MM_INTERNAL_H
#define CPU_MM_INTERNAL_H

#include <paging.h>

/* Physical address range accessible to the system. Sv39 supports 56-bit
 * physical addresses (44-bit PPN + 12-bit offset). On RISC-V there is no DMA
 * zone or HIGHMEM — the kernel direct-maps all of physical RAM, so system
 * allocations can come from anywhere.
 *
 * Set to 56-bit max so the physical allocator sees all installed RAM (e.g. 16
 * GB on SiFive Unmatched, PA 0x80000000–0x47fffffff).
 */
#define CPU_SYSTEM_PADDR_START 0
#define CPU_SYSTEM_PADDR_END ((1UL << 56) - 1)

/* RISC-V can map any physical address */
#define CPU_SYSTEM_PADDR_MUST 0

/* Sv39 supports 4K and 2M (megapage) sizes */
#define CPU_SYSTEM_HAVE_MULTIPLE_PAGESIZES VPS_AUTO

/* Virtual address regions for user programs (Sv39 lower half) */
#define CPU_USER_VADDR_START \
    0x00010000UL /* 64 KB guard for NULL dereferences */
#define CPU_USER_VADDR_END 0x3FFFFFFFUL
#define CPU_SO_VADDR_START 0x38000000UL
#define CPU_SHMEM_VADDR_START 0x20000000UL

/* 1-to-1 mapping: RISC-V uses the kernel's direct-map region (upper half of
 * Sv39 space), established at boot via the kernel page tables. For now,
 * temp_map always goes through explicit page-table mappings.
 */
#define CPU_1TO1_VADDR_BIAS KERNEL_VIRT_BASE
#define CPU_1TO1_IS_VADDR(v) ((uintptr_t) (v) >= KERNEL_VIRT_BASE)
#define CPU_1TO1_IS_PADDR(p) 1

/* Zero a page via memset (no special CPU instruction needed) */
#define CPU_ZERO_PAGE(d, l, f) __builtin_memset((d), 0, (l))

/* RISC-V page tables enforce write protection in all modes */
#define CPU_FAULT_ON_WRITE_WORKS 1

/* Per-address-space CPU state. On RISC-V, satp register holds the root page
 * table PPN + ASID.
 */
struct cpu_mm_as {
    struct cpu_mm_as *next;
    struct cpu_mm_as **prev;
    void
        *pgdir; /* Root page table virtual address (VA==PA with identity map) */
    paddr_t ptroot_paddr; /* Root page table physical address (for satp) */
    uint16_t asid;        /* Address Space ID (for TLB tagging) */
};

/* Sv39 page table entry (64-bit) */
typedef struct {
    uint64_t pte;
} pxe_t;

#define PXE_ADD(p, o) ((pxe_t *) ((uintptr_t) (p) + (o)))
#define PXE_GET(p) ((p)->pte)
#define PXE_SET(p, v) ((p)->pte = (v))
#define PXE_GET_FLAGS(p) ((uint32_t) ((p)->pte & 0x3FF))
#define PXE_SET_FLAGS(p, m, s) ((p)->pte = ((p)->pte & (m)) | (s))

/* Sv39 constants */
#define RISCV_PTE_BITS 3    /* 8 bytes per PTE = 2^3 */
#define RISCV_VPN_BITS 9    /* 512 entries per page table */
#define RISCV_PAGE_LEVELS 3 /* Sv39: 3 levels */

/* PTE physical address mask (bits 53:10 hold PPN) */
#define PTE_PADDR_BITS 0x003FFFFFFFFFFC00ULL

/* PA↔VA conversion for VM code. Uses the kernel's 1-to-1 direct mapping at
 * KERNEL_VIRT_BASE. These are equivalent to PHYS_TO_PTR / PTR_TO_PHYS in
 * runtime.h but defined here for use by roottask code.
 */
#define PA_TO_VA(pa) ((void *) ((uintptr_t) (pa) + CPU_1TO1_VADDR_BIAS))
#define VA_TO_PA(va) ((paddr_t) ((uintptr_t) (va) - CPU_1TO1_VADDR_BIAS))

extern void pgtbl_init(void);

/* System virtual address layout — NYI, placeholder */
#define SYSADDR_BASE 0xFFFFFFC000000000UL
#define MAP_BASE SYSADDR_BASE
#define MAP_SIZE 0x10000000UL

#endif /* CPU_MM_INTERNAL_H */
