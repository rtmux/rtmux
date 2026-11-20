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
 * The walkers read a table through PA_TO_VA(pa), the kernel's direct map from
 * cpu-paging.h; a host test defines its own before including this header.
 */

#ifndef _RISCV_SV39_LOOKUP_H
#define _RISCV_SV39_LOOKUP_H

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <cpu-paging.h>

/* sv39_leaf_size — bytes mapped by a leaf PTE at level (0, 1 or 2). */
static inline uint64_t sv39_leaf_size(int level)
{
    return (uint64_t) 1 << PXSHIFT(level);
}

/* sv39_is_leaf — whether e, a valid entry, is a leaf (any of R/W/X) rather
 * than a pointer to the next table.
 */
static inline int sv39_is_leaf(pte_t e)
{
    return (e & (PTE_R | PTE_W | PTE_X)) != 0;
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
        if (sv39_is_leaf(e)) {
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

/* sv39_alloc_fn — returns the PTE value that links a new, zeroed table (valid
 * bit, the table's PPN and any caller flags), or 0 when none can be had.
 */
typedef pte_t (*sv39_alloc_fn)(void);

/* sv39_walk_slot — walk pgdir down to the L0 slot for va.
 *
 * Returns a pointer to the L0 PTE, which may itself be invalid, or NULL with
 * *errp (when non-NULL) set. A missing interior table is created through alloc
 * when it is non-NULL, otherwise the walk ends with ENOENT. A superpage leaf at
 * L2 or L1 already covers va, and its PPN names mapped memory, not a table, so
 * the walk stops there instead of descending, with EEXIST whether or not alloc
 * is set, so a caller that unmaps or re-protects tells a covering superpage
 * apart from a missing table (ENOENT). ENOMEM when alloc returns 0.
 */
static inline pte_t *sv39_walk_slot(pagetable_t pgdir,
                                    uintptr_t va,
                                    sv39_alloc_fn alloc,
                                    int *errp)
{
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pgdir[PX(level, va)];
        pte_t e = *pte; /* one load: the checks and the descent agree */

        if (!(e & PTE_V)) {
            if (!alloc) {
                if (errp)
                    *errp = ENOENT;
                return NULL;
            }
            e = alloc();
            if (!e) {
                if (errp)
                    *errp = ENOMEM;
                return NULL;
            }
            *pte = e;
        } else if (sv39_is_leaf(e)) {
            if (errp)
                *errp = EEXIST;
            return NULL;
        }
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(e));
    }
    return &pgdir[PX(0, va)];
}

/* What sv39_manip_pages does to each page of its range. */
enum sv39_manip_op {
    SV39_MANIP_MAP,      /* store a leaf: frame and bits, one frame per page */
    SV39_MANIP_UNMAP,    /* clear a valid leaf */
    SV39_MANIP_PROT,     /* swap a valid leaf's flag bits, keep the rest */
    SV39_MANIP_PREALLOC, /* only make the tables a MAP would need */
};

/* sv39_manip_pages — apply op to the 4 KiB pages in pgdir that start between
 * *startp and end, end excluded when it is page aligned.
 *
 * The one loop behind cpu_pte_manipulate, header-only so the host test runs the
 * kernel's own error mapping. MAP and PREALLOC create tables through alloc and
 * report what sv39_walk_slot did: ENOMEM, or EEXIST over a superpage. UNMAP and
 * PROT never create a table: a missing one holds nothing to change, so it is
 * skipped, but EEXIST over a superpage is reported, since neither can act on
 * part of one. PROT keeps the PPN and the attribute bits above it (Svpbmt,
 * Svnapot) and replaces bits 0..9 with bits.
 *
 * Progress is kept in the caller's variables after every page: *startp is the
 * first page not done and, for MAP, *paddrp the frame it gets, so on an error
 * they name what was done and a caller can undo [first, *startp) or resume.
 * *changedp is set to 1 once a PTE has been written.
 */
static inline int sv39_manip_pages(pagetable_t pgdir,
                                   enum sv39_manip_op op,
                                   uintptr_t *startp,
                                   uintptr_t end,
                                   uint64_t *paddrp,
                                   uint64_t bits,
                                   sv39_alloc_fn alloc,
                                   int *changedp)
{
    int create = op == SV39_MANIP_MAP || op == SV39_MANIP_PREALLOC;

    for (uintptr_t va = *startp; va < end; va += PGSIZE, *startp = va) {
        int err = create ? ENOMEM : ENOENT;
        pte_t *pte = sv39_walk_slot(pgdir, va, create ? alloc : NULL, &err);

        if (!pte) {
            if (create || err == EEXIST)
                return err;
            continue; /* no table, so no leaf to unmap or re-protect */
        }
        switch (op) {
        case SV39_MANIP_MAP:
            *pte = PA2PTE(*paddrp) | bits;
            *paddrp += PGSIZE;
            *changedp = 1;
            break;
        case SV39_MANIP_UNMAP:
        case SV39_MANIP_PROT:
            if (!(*pte & PTE_V))
                break;
            *pte = op == SV39_MANIP_UNMAP ? 0
                                          : (*pte & ~PTE_FLAG_BITS) | bits;
            *changedp = 1;
            break;
        case SV39_MANIP_PREALLOC:
            break;
        }
    }
    return 0;
}

/* Kernel entry point for sv39_lookup_leaf (arch/riscv/cpu-pte.c). */
pte_t *cpu_pte_lookup(pagetable_t pgdir,
                      uintptr_t vaddr,
                      int *levelp,
                      pte_t *ep);

#endif /* _RISCV_SV39_LOOKUP_H */
