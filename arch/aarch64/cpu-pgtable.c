/* SPDX-License-Identifier: Apache-2.0 */

#include <core/globals.h>
#include <paging.h>

pagetable_t cpu_pgdir_create(void)
{
    pagetable_t pgdir = (pagetable_t) _scalloc_pgalign(PGSIZE);
    if (!pgdir)
        return NULL;

    extern paddr_t page_directory;
    pagetable_t kern_pgdir = PHYS_TO_PTR(page_directory);

    for (int i = 256; i < 512; i++) {
        if ((kern_pgdir[i] & PTE_V) == 0)
            crash_halt(
                "cpu_pgdir_create: kernel L2[%d] is empty — "
                "init_mmu prealloc missed it or a runtime "
                "kernel map populated a fresh slot\n",
                i);
    }

    memcpy(&pgdir[256], &kern_pgdir[256], 256 * sizeof(pte_t));

    pgdir[1] = kern_pgdir[1];

    return pgdir;
}

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
            *pte = PA2PTE(PTR_TO_PHYS(pgdir)) | 3;
        }
    }
    return &pgdir[PX(0, va)];
}

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
        *pte = PA2PTE(pa) | aarch64_page_attrs((unsigned) pte_flags);
        if (a == last)
            break;
        a += PGSIZE;
        pa += PGSIZE;
    }
    return 0;
}

void cpu_pgdir_free(pagetable_t pgdir)
{
    pte_t l2_entry = pgdir[0];
    if ((l2_entry & PTE_V) && !cpu_pte_leaf(l2_entry, 2)) {
        pagetable_t l1 = (pagetable_t) PHYS_TO_PTR(PTE2PA(l2_entry));
        for (int j = 0; j < 512; j++) {
            pte_t l1_entry = l1[j];
            if ((l1_entry & PTE_V) && !cpu_pte_leaf(l1_entry, 1)) {
                _sfree(PHYS_TO_PTR(PTE2PA(l1_entry)), PGSIZE);
            }
        }
        _sfree(l1, PGSIZE);
    }
    _sfree(pgdir, PGSIZE);
}

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

void kernel_guard_page(uintptr_t va)
{
    uint64_t root;
    __asm__ volatile("mrs %0,ttbr1_el1" : "=r"(root));
    pagetable_t table = PHYS_TO_PTR(root & 0x0000fffffffff000UL);
    for (int level = 2; level > 0; level--) {
        pte_t *p = &table[PX(level, va)];
        if (cpu_pte_leaf(*p, level)) {
            if (level != 1)
                crash("unexpected kernel block size");
            pagetable_t leaf = _scalloc_pgalign(PGSIZE);
            if (!leaf)
                crash("guard page allocation");
            pte_t old = *p;
            for (unsigned j = 0; j < 512; j++)
                leaf[j] = (PTE2PA(old) + j * 4096) |
                          (old & ~0x0000fffffffff000UL) | 2;
            *p = 0;
            cpu_tlb_flush();
            *p = PTR_TO_PHYS(leaf) | 3;
        }
        table = PHYS_TO_PTR(PTE2PA(*p));
    }
    table[PX(0, va)] = 0;
    cpu_tlb_flush();
}
