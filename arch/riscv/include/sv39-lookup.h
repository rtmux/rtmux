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
#include "pte-op.h"

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

/* sv39_is_mark — whether e is an invalid entry carrying the PTE_SW_BUS mark
 * (pages past the end of their object: SIGBUS).
 */
static inline int sv39_is_mark(pte_t e)
{
    return (e & (PTE_V | PTE_SW_BUS)) == PTE_SW_BUS;
}

/* sv39_alloc_fn — returns the PTE value that links a new, zeroed table (valid
 * bit, the table's PPN and any caller flags), or 0 when none can be had.
 */
typedef pte_t (*sv39_alloc_fn)(void);

/* sv39_walk_to — walk pgdir down to the entry at level target (1 or 0) for va.
 *
 * Returns a pointer to that entry, which may itself be invalid, or NULL with
 * *errp (when non-NULL) set. A missing interior table is created through alloc
 * when it is non-NULL, otherwise the walk ends with ENOENT. A table created
 * under an invalid entry that carries the PTE_SW_BUS mark (a whole 2 MiB past
 * the end of its object) starts with every entry marked, so splitting the mark
 * keeps it on the pages the caller does not change. A superpage leaf above
 * target already covers va, and its PPN names mapped memory, not a table, so
 * the walk stops there instead of descending, with EEXIST whether or not alloc
 * is set, so a caller that unmaps or re-protects tells a covering superpage
 * apart from a missing table (ENOENT). ENOMEM when alloc returns 0.
 */
static inline pte_t *sv39_walk_to(pagetable_t pgdir,
                                  uintptr_t va,
                                  int target,
                                  sv39_alloc_fn alloc,
                                  int *errp)
{
    for (int level = 2; level > target; level--) {
        pte_t *pte = &pgdir[PX(level, va)];
        pte_t e = *pte; /* one load: the checks and the descent agree */

        if (!(e & PTE_V)) {
            pte_t t;

            if (!alloc) {
                if (errp)
                    *errp = ENOENT;
                return NULL;
            }
            t = alloc();
            if (!t) {
                if (errp)
                    *errp = ENOMEM;
                return NULL;
            }
            if (e & PTE_SW_BUS) {
                pagetable_t table = (pagetable_t) PA_TO_VA(PTE2PA(t));

                for (int i = 0; i <= PXMASK; i++)
                    table[i] = PTE_SW_BUS;
            }
            *pte = e = t;
        } else if (sv39_is_leaf(e)) {
            if (errp)
                *errp = EEXIST;
            return NULL;
        }
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(e));
    }
    return &pgdir[PX(target, va)];
}

/* sv39_walk_slot — sv39_walk_to the level-zero slot for va. */
static inline pte_t *sv39_walk_slot(pagetable_t pgdir,
                                    uintptr_t va,
                                    sv39_alloc_fn alloc,
                                    int *errp)
{
    return sv39_walk_to(pgdir, va, 0, alloc, errp);
}

/* sv39_manip_pages — apply op to the 4 KiB pages in pgdir that start between
 * *startp and end, end excluded when it is page aligned.
 *
 * The one loop behind cpu_pte_manipulate, header-only so the host test runs the
 * kernel's own error mapping. MAP and PREALLOC create tables through alloc and
 * report what sv39_walk_slot did: ENOMEM, or EEXIST over a superpage. UNMAP and
 * PROT never create a table: a missing one holds nothing to change, so it is
 * skipped, but EEXIST over a superpage is reported, since neither can act on
 * part of one. PROT keeps the PPN and the attribute bits above it (Svpbmt,
 * Svnapot) and replaces bits 0..9 with bits. BAD creates tables like MAP and
 * writes the invalid PTE_SW_BUS mark over whatever each slot held, or, for a
 * whole empty 2 MiB region, into its level-one slot; UNMAP clears the mark at
 * either level (splitting a level-one mark it covers in part), and PROT leaves
 * it as it is.
 *
 * Progress is kept in the caller's variables after each level-zero table run or
 * level-one mark: *startp is the first page not done and, for MAP, *paddrp the
 * frame it gets, so on an error they name what was done and a caller can undo
 * [first, *startp) or resume.
 * *changedp is set to 1 once a PTE has been written.
 */
static inline int sv39_manip_pages(pagetable_t pgdir,
                                   enum pte_op op,
                                   uintptr_t *startp,
                                   uintptr_t end,
                                   uint64_t *paddrp,
                                   uint64_t bits,
                                   sv39_alloc_fn alloc,
                                   int *changedp)
{
    int create = op == PTE_OP_MAP || op == PTE_OP_BAD || op == PTE_OP_PREALLOC;

    const uintptr_t mib2 = (uintptr_t) sv39_leaf_size(1);
    uintptr_t va = *startp;
    while (va < end) {
        /* A whole 2 MiB region: every page of it starts below end, whether end
         * is one past the range or its last byte.
         */
        int whole = !(va & (mib2 - 1)) && end - va > mib2 - PGSIZE;
        int err = 0;
        pte_t *pte;

        /* BAD over a whole empty 2 MiB region: one mark in its level-one
         * slot, and no level-zero table, so marking costs at most one table
         * per 2 MiB (the partial regions at the ends). An invalid entry
         * rewritten needs no TLB flush.
         */
        if (op == PTE_OP_BAD && whole) {
            pte = sv39_walk_to(pgdir, va, 1, alloc, &err);
            if (!pte)
                return err;
            if (!(*pte & PTE_V)) {
                *pte = PTE_SW_BUS;
                va += mib2;
                *startp = va;
                continue;
            }
            if (sv39_is_leaf(*pte))
                return EEXIST;
            /* A table is there: mark its entries below. */
        } else if (op == PTE_OP_BAD) {
            /* Part of a region its level-one mark covers already: nothing to
             * split, since the pages are marked.
             */
            pte = sv39_walk_to(pgdir, va, 1, NULL, NULL);
            if (pte && sv39_is_mark(*pte)) {
                va = (va | (mib2 - 1)) + 1;
                *startp = va;
                continue;
            }
        }
        pte = sv39_walk_slot(pgdir, va, create ? alloc : NULL, &err);
        if (!pte && err == ENOENT && op == PTE_OP_UNMAP) {
            /* No table, but a level-one mark may cover va: an unmap clears
             * it, whole when the range covers its region, else by splitting
             * it into a table of marks; with no table to be had, the whole
             * mark goes (the rest of its region then faults SIGSEGV, not
             * SIGBUS).
             */
            pte_t *l1 = sv39_walk_to(pgdir, va, 1, NULL, NULL);

            if (l1 && sv39_is_mark(*l1) &&
                (whole || !(pte = sv39_walk_slot(pgdir, va, alloc, NULL)))) {
                *l1 = 0;
                va = (va | (mib2 - 1)) + 1;
                *startp = va;
                continue;
            }
        }
        if (!pte && (create || err == EEXIST))
            return err;
        /* The remaining slots in this level-zero table are contiguous. */
        size_t pages = (end - va - 1) / PGSIZE + 1;
        size_t slots = PXMASK + 1 - PX(0, va);
        if (pages > slots)
            pages = slots;
        if (pte && op != PTE_OP_PREALLOC) {
            for (size_t i = 0; i < pages; i++) {
                if (op == PTE_OP_MAP) {
                    pte[i] = PA2PTE(*paddrp) | bits;
                    *paddrp += PGSIZE;
                } else if (op == PTE_OP_BAD || op == PTE_OP_UNMAP) {
                    /* Rewriting an invalid entry needs no TLB flush: no
                     * hart can hold a translation for it.
                     */
                    pte_t old = pte[i];

                    pte[i] = op == PTE_OP_BAD ? PTE_SW_BUS : 0;
                    if (!(old & PTE_V))
                        continue;
                } else if (pte[i] & PTE_V) {
                    pte[i] = (pte[i] & ~PTE_FLAG_BITS) | bits;
                } else {
                    continue;
                }
                *changedp = 1;
            }
        }
        va += pages * PGSIZE;
        *startp = va;
    }
    return 0;
}

/* sv39_past_end — whether va lies on a page marked PTE_SW_BUS: part of a
 * mapping, but past the end of the object it maps. The mark sits in the first
 * invalid entry of the walk, at level zero or, for a whole 2 MiB region, at
 * level one; a superpage or an unmarked invalid entry says no.
 */
static inline int sv39_past_end(pagetable_t pgdir, uintptr_t va)
{
    for (int level = 2; level >= 0; level--) {
        pte_t e = pgdir[PX(level, va)];

        if (!(e & PTE_V))
            return (e & PTE_SW_BUS) != 0;
        if (sv39_is_leaf(e))
            return 0;
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(e));
    }
    return 0;
}

/* Kernel entry point for sv39_lookup_leaf (arch/riscv/cpu-pte.c). */
pte_t *cpu_pte_lookup(pagetable_t pgdir,
                      uintptr_t vaddr,
                      int *levelp,
                      pte_t *ep);

#endif /* _RISCV_SV39_LOOKUP_H */
