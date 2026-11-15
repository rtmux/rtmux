/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Read-only Sv39 leaf lookup and leaf address arithmetic.
 *
 * A leaf PTE can sit at any of the three levels: L0 maps 4 KiB, L1 maps 2 MiB,
 * and L2 maps 1 GiB. The physical address of a VA is the leaf's base plus the
 * VA's offset within that leaf, so the offset mask depends on the level the
 * walk stopped at. Kept header-only so the host unit test runs the same walk
 * and arithmetic the kernel does.
 *
 * The walker is defined only when the includer has PA_TO_VA(pa), which turns a
 * table's physical address into a pointer the walker can read. Other callers go
 * through cpu_pte_lookup.
 */

#ifndef _RISCV_SV39_LOOKUP_H
#define _RISCV_SV39_LOOKUP_H

#include <stddef.h>
#include <stdint.h>
#include <cpu-paging.h>

/* sv39_leaf_size — bytes mapped by a leaf PTE at level (0, 1 or 2). */
static inline uint64_t sv39_leaf_size(int level)
{
    return (uint64_t) 1 << PXSHIFT(level);
}

/* sv39_leaf_ok — whether e, a valid entry with R/W/X at level, is a leaf the
 * hardware translates through. W without R is reserved, and a superpage whose
 * PPN is not aligned to its size is misaligned; the hardware faults on both.
 */
static inline int sv39_leaf_ok(pte_t e, int level)
{
    return (e & (PTE_R | PTE_W)) != PTE_W &&
           !(PTE2PA(e) & (sv39_leaf_size(level) - 1));
}

/* sv39_leaf_paddr — physical address of va under leaf PTE value pte at level.
 *
 * The leaf's base supplies the bits above the leaf size and the VA the bits
 * below it; pte must be a leaf sv39_lookup_leaf accepted at level. When lenp is
 * non-NULL it receives the bytes from va to the end of the leaf.
 */
static inline uint64_t sv39_leaf_paddr(pte_t pte,
                                       int level,
                                       uintptr_t va,
                                       size_t *lenp)
{
    uint64_t size = sv39_leaf_size(level);
    uint64_t off = va & (size - 1);

    if (lenp)
        *lenp = (size_t) (size - off);
    return PTE2PA(pte) | off;
}

#ifdef PA_TO_VA
/* sv39_lookup_leaf — read-only walk of pgdir for va.
 *
 * Returns a pointer to the valid leaf PTE that maps va, or NULL if va is not
 * mapped or the leaf is one the hardware would fault on. When levelp is
 * non-NULL and a leaf is found, *levelp receives the level that leaf sits at: 2
 * for a 1 GiB page, 1 for 2 MiB, 0 for 4 KiB. When ep is non-NULL it receives
 * the entry's value as the walk read and checked it.
 *
 * The entry can change once the walk returns, so a caller that acts on it uses
 * *ep rather than reading the returned pointer again.
 */
static inline pte_t *sv39_lookup_leaf(pagetable_t pgdir,
                                      uintptr_t va,
                                      int *levelp,
                                      pte_t *ep)
{
    for (int level = 2; level >= 0; level--) {
        pte_t *pte = &pgdir[PX(level, va)];
        pte_t e = *pte; /* one load: the checks and the descent agree */

        if (!(e & PTE_V))
            return NULL;
        /* A leaf has any of R/W/X; an interior entry has none. */
        if (e & (PTE_R | PTE_W | PTE_X)) {
            if (!sv39_leaf_ok(e, level))
                return NULL;
            if (levelp)
                *levelp = level;
            if (ep)
                *ep = e;
            return pte;
        }
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(e));
    }
    return NULL;
}
#endif

/* Kernel entry point for sv39_lookup_leaf (arch/riscv/cpu-pte.c). */
pte_t *cpu_pte_lookup(pagetable_t pgdir,
                      uintptr_t vaddr,
                      int *levelp,
                      pte_t *ep);

#endif /* _RISCV_SV39_LOOKUP_H */
