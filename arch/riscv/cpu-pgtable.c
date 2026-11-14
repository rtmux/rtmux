/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* cpu-pgtable.c — Sv39 page table management for user address spaces
 *
 * Provides functions to create, populate, and free per-process page tables.
 * Used by the ELF loader (initramfs-elf.c) to set up virtual address spaces for
 * user binaries.
 *
 * The page table structure follows RISC-V Sv39: three levels of 512-entry
 * tables (L2 → L1 → L0), each entry 8 bytes.
 *
 * The kernel runs at high VA (KERNEL_VIRT_BASE + PA). All pointers returned by
 * the allocator are high VA; PTEs store physical addresses. Conversions use
 * PTR_TO_PHYS() and PHYS_TO_PTR().
 *
 * License: Apache 2.0
 */

#include <core/globals.h>
#include <paging.h>

/* Leaf-PTE memory-type bits (see cpu-paging.h). Written once by startup
 * init_mmu() before any leaf PTE is built; zero unless the core is T-Head with
 * MAEE enabled.
 */
uint64_t riscv_pte_attr_mem;
uint64_t riscv_pte_attr_io;

/* cpu_pgdir_create — allocate a user root page table with kernel mappings.
 *
 * Allocates a zeroed 4KB page, then copies L2 entries 1..511 from the boot
 * (kernel) page table so that kernel code (trap handlers, etc.) is reachable
 * when this page table is active.
 *
 * Entry 0 covers user VA 0..0x3FFFFFFF and is left zeroed — the caller
 * populates it with user mappings via cpu_map_pages().
 *
 * Returns the pagetable_t (virtual pointer), or NULL on failure.
 */
pagetable_t cpu_pgdir_create(void)
{
    pagetable_t pgdir = (pagetable_t) _scalloc_pgalign(PGSIZE);
    if (!pgdir)
        return NULL;

    /* Get kernel root page table from the current SATP */
    uint64_t satp = r_satp();
    paddr_t kern_ptroot_pa = (satp & 0xFFFFFFFFFFFUL) << PGSHIFT;
    pagetable_t kern_pgdir = (pagetable_t) PHYS_TO_PTR(kern_ptroot_pa);

    /* Sv39 splits the address space into 512 L2 slots of 1 GiB each. With
     * KERNEL_VIRT_BASE = 0xffffffc000000000, slots 256..511 cover the kernel
     * half and slots 0..255 cover user-space.
     *
     * The startup-time prealloc_kernel_l2 step in init_mmu() ensures every
     * kernel L2 slot (256..511) holds a valid L1 page, so the copy below
     * installs PTEs that share those L1 pages with the kernel pgdir. Any future
     * kernel mapping that modifies entries inside one of those L1 pages is
     * therefore visible here too.
     *
     * If any kernel L2 slot is still empty at this point, the prealloc was
     * bypassed or the kernel went on to allocate a NEW L1 page after startup;
     * either way running the kernel under this user satp and touching that VA
     * would store-page-fault during trap save (the "romfs hang" fingerprint).
     * Fail loudly here instead of silently propagating the bug.
     *
     * Important: only the kernel half plus the syspage slot are copied. An
     * earlier version of this code copied entries 1..511 (including all
     * user-half slots 1..255). That was harmless for processes that never
     * touched L2[1..255]. N we put roottask at L2[3] (ROOTTASK_LOAD_BASE =
     * 0xC0000000), so the only user-half slot we still need from the boot pgdir
     * is L2[1], which holds the syspage (RTMUX_SYSPAGE_UADDR=0x40001000) +
     * cpupage + kcall trampoline shared by every U-mode process. Slots 0, 2,
     * 3..255 stay zeroed (slot 0 is owned by the per-process binary loader,
     * slot 3 is owned by roottask, the rest are private heap/mmap range).
     */
    for (int i = 256; i < 512; i++) {
        if ((kern_pgdir[i] & PTE_V) == 0)
            crash_halt(
                "cpu_pgdir_create: kernel L2[%d] is empty — "
                "init_mmu prealloc missed it or a runtime "
                "kernel map populated a fresh slot\n",
                i);
    }

    memcpy(&pgdir[256], &kern_pgdir[256], 256 * sizeof(pte_t));

    /* Share L2[1] (syspage / cpupage / kcall trampoline) with every user
     * process, exactly as cpu-paging.h's RTMUX_SYSPAGE_UADDR comment promises
     * ("copied from the boot page table into every new process page table").
     */
    pgdir[1] = kern_pgdir[1];

    return pgdir;
}

/* walk — traverse the page table, optionally creating intermediate levels.
 *
 * Returns a pointer to the L0 PTE for the given VA, or NULL if alloc is false
 * and the mapping doesn't exist.
 */
static pte_t *walk(pagetable_t pgdir, uintptr_t va, int alloc)
{
    if (va >= MAXVA)
        return NULL;

    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pgdir[PX(level, va)];
        if (*pte & PTE_V) {
            pgdir = (pagetable_t) PHYS_TO_PTR(PTE2PA(*pte));
        } else {
            if (!alloc)
                return NULL;
            pgdir = (pagetable_t) _scalloc_pgalign(PGSIZE);
            if (!pgdir)
                return NULL;
            *pte = PA2PTE(PTR_TO_PHYS(pgdir)) | PTE_V;
        }
    }
    return &pgdir[PX(0, va)];
}

/* cpu_map_pages — map VA → PA with given PTE flags in the page table.
 *
 * Maps 4KB pages from [va, va+size) to [pa, pa+size). Both va and pa must be
 * page-aligned, size must be > 0.
 * Returns 0 on success, -1 on failure.
 */
int cpu_map_pages(pagetable_t pgdir,
                  uintptr_t va,
                  paddr_t pa,
                  size_t size,
                  uint64_t pte_flags)
{
    if (size == 0) {
        kprintf("cpu_map_pages: size == 0\n");
        return -1;
    }

    uintptr_t a = PGROUNDDOWN(va);
    uintptr_t last = PGROUNDDOWN(va + size - 1);

    for (;;) {
        pte_t *pte = walk(pgdir, a, 1);
        if (!pte)
            return -1;
        if (*pte & PTE_V) {
            kprintf("cpu_map_pages: remap at va=%lx\n", (unsigned long) a);
            return -1;
        }
        /* Callers (the initramfs ELF loader) only map RAM */
        *pte = PA2PTE(pa) | pte_flags | riscv_pte_attr_mem;
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

/* cpu_pgdir_free — free a page table tree (walk and free all levels).
 *
 * Frees the page table pages themselves, NOT the physical pages that were
 * mapped. Caller must free those separately if needed.
 */
void cpu_pgdir_free(pagetable_t pgdir)
{
    /* L2: only entry 0 (user VA range 0..0x3FFFFFFF) is per-process. Entries
     * 1..511 hold kernel-shared L1 pointers (the kernel-L2 prealloc copies them
     * from the boot pgdir into every user pgdir). Walking & freeing those would
     * corrupt every other process's page tables. Limit the walk to user-half.
     *
     * For the L1 pointed at by L2[0]: each L1 entry is either a leaf (2 MiB
     * megapage, no L0 to free) or points to an L0 table. Free the L0 in the
     * latter case, then the L1, then the pgdir itself.
     */
    pte_t l2_entry = pgdir[0];
    if ((l2_entry & PTE_V) && !(l2_entry & (PTE_R | PTE_W | PTE_X))) {
        pagetable_t l1 = (pagetable_t) PHYS_TO_PTR(PTE2PA(l2_entry));
        for (int j = 0; j < 512; j++) {
            pte_t l1_entry = l1[j];
            if ((l1_entry & PTE_V) && !(l1_entry & (PTE_R | PTE_W | PTE_X))) {
                _sfree(PHYS_TO_PTR(PTE2PA(l1_entry)), PGSIZE);
            }
        }
        _sfree(l1, PGSIZE);
    }
    _sfree(pgdir, PGSIZE);
}

/* cpu_va2pa — look up a VA in a page table, return PA (or 0 if unmapped). */
paddr_t cpu_va2pa(pagetable_t pgdir, uintptr_t va)
{
    if (va >= MAXVA)
        return 0;

    pte_t *pte = walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_V))
        return 0;

    paddr_t pa = PTE2PA(*pte);
    return pa | (va & (PGSIZE - 1));
}
