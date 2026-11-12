/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * mm_map_xfer — kernel-resident IOV remapper for cross-as message transfer
 * (xfermsg's dst-path / src-path). Replaces the call into as_map_xfer in
 * roottask text, which is unfetchable from S-mode now that roottask runs U-mode
 * (PTE_U=1 on its pages).
 *
 * Contract (matches vm_ops->map_xfer):
 *   Translate "parts" user-VA IOV entries from process "prp" (which
 *   may have a different satp than the active as "actprp") into
 *   up to "*pnparts" kernel-direct-map IOV entries in "niov".
 *
 *   Returns: bytes mapped, 0 if exhausted, -1 on error.
 *   Updates *piov / *pparts / *poff / *pnparts to reflect progress.
 *
 * Only the page-table walker differs from xferiov's plain memcpy path — once
 * the user VAs are translated to kernel-direct-map VAs (PA + KERNEL_VIRT_BASE),
 * xferiov copies them safely under the active satp. This keeps cross-as IPC
 * copy deterministic without a temporary satp switch.
 */

#include <core/globals.h>
#include "as.h"         /* address_t, pagetable_t, pte_t, paddr_t */
#include <cpu-paging.h> /* PX / PXSHIFT / PTE2PA / PTE_V/R/W/X / PGSIZE */
#include <unistd.h>     /* iov_t, GETIOVBASE / GETIOVLEN / SETIOV */

/* vm-hooks.h flags — duplicated here to avoid header churn */
#ifndef MAPADDR_FLAGS_SYSPRP
#define MAPADDR_FLAGS_SYSPRP 0x2
#endif
#ifndef MAPADDR_FLAGS_IOVKERNEL
#define MAPADDR_FLAGS_IOVKERNEL 0x8000
#endif

/* WITHIN_BOUNDARY lives in macros.h (already pulled by core/globals.h) but is
 * namespaced for arch-specific overrides; the canonical RISC-V definition is
 * "addr/last must both be < boundary".
 */
#ifndef WITHIN_BOUNDARY
#define WITHIN_BOUNDARY(addr, last, bound)       \
    ((uintptr_t) (addr) < (uintptr_t) (bound) && \
     (uintptr_t) (last) < (uintptr_t) (bound))
#endif

/* sv39_va_to_pa — read-only walk of an Sv39 page table.
 *
 * Returns the physical address backing "va" and the number of contiguous bytes
 * remaining in that mapping (4 KiB for L0 leaf, 2 MiB for L1 megapage, 1 GiB
 * for L2 gigapage); (paddr_t)-1 on miss.
 *
 * Mirrors servers/roottask/mem/riscv/as-map-xfer.c's helper, kept inline here
 * so the kernel doesn't depend on roottask text.
 */
static paddr_t sv39_va_to_pa(pagetable_t pgdir, uintptr_t va, size_t *contig)
{
    /* PXSHIFT(level) is 12 + 9*level, so every shift below depends on level
     * staying in 1..2. The bound is written into the loop header, but a prover
     * will not read it off the header for a shift inside the body.
     */
    /*@ loop invariant 0 <= level <= 2;
        loop assigns level, pgdir;
        loop variant level;
     */
    for (int level = 2; level > 0; level--) {
        pte_t pte = pgdir[PX(level, va)];
        if (!(pte & PTE_V))
            return (paddr_t) -1;
        if (cpu_pte_leaf(pte, level)) {
            size_t pgsize = 1UL << PXSHIFT(level);
            paddr_t pa = PTE2PA(pte) | (va & (pgsize - 1));
            if (contig)
                *contig = pgsize - (va & (pgsize - 1));
            return pa;
        }
        pgdir = (pagetable_t) ((uintptr_t) PTE2PA(pte) + KERNEL_VIRT_BASE);
    }

    pte_t pte = pgdir[PX(0, va)];
    if (!(pte & PTE_V))
        return (paddr_t) -1;

    paddr_t pa = PTE2PA(pte) | (va & (PGSIZE - 1));
    if (contig)
        *contig = PGSIZE - (va & (PGSIZE - 1));
    return pa;
}

/* fetch_iov — copy one iov_t entry out of prp's address space into "out".
 *
 * When "xlate" is true the entry lives at a user VA whose backing page must be
 * walked; the 16-byte entry can straddle a page boundary (an array not aligned
 * to sizeof(iov_t) within its first page), so translate and copy per page. When
 * false (IOVKERNEL, or a kernel-direct VA) the entry is read directly.
 *
 * This is what lets the caller walk the IOV array by user VA and re-translate
 * every entry: consecutive user pages are not physically consecutive, so a
 * kernel-direct alias of the array's first page cannot be advanced across a
 * page boundary.
 *
 * When "bound" is non-zero the entry's own VA range is range-checked against it
 * first: the array cursor is a user VA the caller advances by sizeof(iov_t),
 * and a large part count could walk it past the process boundary into the
 * high-half kernel map (present in every AS), where the page walk below would
 * succeed and leak kernel memory. A zero "bound" waives the check
 * (MAPADDR_FLAGS_SYSPRP).
 *
 * "cache_va"/"cache_pa" carry the last translated 4 KiB page across calls (seed
 * cache_va = (uintptr_t)-1). Consecutive entries usually share a page — up to
 * 256 iov_t per 4 KiB — so this reuses one walk per page instead of one per
 * entry, keeping the old translate-once cost while re-walking on a page
 * crossing (which is exactly what the per-page correctness rule needs).
 *
 * Returns 0 on success, -1 on a translation miss or out-of-bounds entry VA.
 */
static int fetch_iov(pagetable_t pgdir,
                     uintptr_t uva,
                     bool xlate,
                     uintptr_t bound,
                     uintptr_t *cache_va,
                     paddr_t *cache_pa,
                     iov_t *out)
{
    if (!xlate) {
        *out = *(const iov_t *) uva;
        return 0;
    }

    if (bound) {
        uintptr_t last = uva + sizeof(iov_t) - 1;
        if (last < uva || !WITHIN_BOUNDARY(uva, last, bound))
            return -1;
    }

    uint8_t *dst = (uint8_t *) out;
    size_t remaining = sizeof(iov_t);
    while (remaining) {
        uintptr_t page = uva & ~((uintptr_t) PGSIZE - 1);
        size_t pgoff = uva & (PGSIZE - 1);
        paddr_t pa;

        if (page == *cache_va) {
            pa = *cache_pa + pgoff;
        } else {
            pa = sv39_va_to_pa(pgdir, uva, NULL);
            if (pa == (paddr_t) -1)
                return -1;
            *cache_va = page;
            *cache_pa = pa - pgoff; /* 4 KiB page-base PA */
        }

        /* Copy only to the 4 KiB page end; the next page re-checks the cache (a
         * 16-byte entry that straddles a page boundary needs both walks).
         */
        size_t n = PGSIZE - pgoff;
        if (n > remaining)
            n = remaining;
        memcpy(dst, (const void *) ((uintptr_t) pa + KERNEL_VIRT_BASE), n);
        dst += n;
        uva += n;
        remaining -= n;
    }
    return 0;
}

/* mm_map_xfer — translate a process's user-VA IOV list into kernel- direct-map
 * IOV entries for cross-as message transfer.
 *
 * Walks prp's Sv39 page table to convert up to *pnparts user-VA IOV entries
 * (starting *poff bytes into *piov) into kernel-direct-map IOV entries in niov;
 * kernel-VA entries pass through untranslated. Updates *piov / *pparts / *poff
 * / *pnparts to reflect progress. Parameters:
 *   actprp  : active address space (unused; satp is set by the caller)
 *   prp     : process whose page table backs the source IOV VAs
 *   piov    : in/out pointer to the current IOV entry, advanced on return
 *   pparts  : in/out count of remaining IOV entries
 *   poff    : in/out byte offset into the current IOV entry
 *   niov    : output array of translated kernel-direct-map IOV entries
 *   pnparts : in/out capacity of niov, set to entries produced on return
 *   flags   : MAPADDR_FLAGS_* (SYSPRP bypasses bounds, IOVKERNEL skips
 *             translating the IOV pointer itself)
 * Return value: bytes mapped, 0 if exhausted, -1 on error.
 */
int mm_map_xfer(process_t *actprp,
                process_t *prp,
                iov_t **piov,
                int *pparts,
                int *poff,
                iov_t *niov,
                int *pnparts,
                unsigned flags)
{
    address_t *adp;
    pagetable_t pgdir;
    iov_t *iov;
    iov_t cur;
    uintptr_t uiov;
    int parts, nparts, nparts_max, iov_advances, bytes;
    size_t off;
    uintptr_t bound;
    uintptr_t iov_bound;
    uintptr_t cache_va = (uintptr_t) -1;
    paddr_t cache_pa = 0;
    bool xlate_iov;

    (void) actprp; /* satp is set by the caller; we walk prp's table */

    if (!prp || !(adp = prp->memory))
        return -1;
    pgdir =
        (pagetable_t) ((uintptr_t) adp->cpu.ptroot_paddr + KERNEL_VIRT_BASE);

    iov = *piov;
    parts = *pparts;
    bound = prp->boundary_addr;
    nparts_max = *pnparts;
    iov_advances = 0;

    /* The IOV array itself may live in "prp"'s user space (when xfermsg passes
     * a non-IOVKERNEL iov from a remote process). Carry the user VA in "uiov"
     * and fetch each entry through fetch_iov(), which re-translates per entry —
     * the array can span several user pages that are not physically contiguous,
     * so a one-shot translation of its first page cannot be walked with iov++
     * across a page boundary.
     */
    xlate_iov = !(flags & MAPADDR_FLAGS_IOVKERNEL) &&
                (uintptr_t) iov < KERNEL_VIRT_BASE;
    iov_bound = (flags & MAPADDR_FLAGS_SYSPRP) ? 0 : bound;
    uiov = (uintptr_t) iov;
    if (fetch_iov(pgdir, uiov, xlate_iov, iov_bound, &cache_va, &cache_pa,
                  &cur) < 0)
        return -1;

    /* Skip over the initial offset within the IOV list. */
    off = (size_t) *poff;
    while (off >= GETIOVLEN(&cur)) {
        size_t len = GETIOVLEN(&cur);
        if (--parts == 0)
            return 0;
        off -= len;
        uiov += sizeof(iov_t);
        iov_advances++;
        if (fetch_iov(pgdir, uiov, xlate_iov, iov_bound, &cache_va, &cache_pa,
                      &cur) < 0)
            return -1;
    }

    uintptr_t addr = (uintptr_t) GETIOVBASE(&cur) + off;
    size_t size = GETIOVLEN(&cur) - off;

    if (size != 0) {
        uintptr_t last = addr + size - 1;
        if (addr > last || (!(flags & MAPADDR_FLAGS_SYSPRP) &&
                            !WITHIN_BOUNDARY(addr, last, bound)))
            return -1;
    }

    nparts = 0;
    bytes = 0;

    while (nparts < nparts_max) {
        if (size == 0) {
            if (--parts <= 0)
                break;
            uiov += sizeof(iov_t);
            iov_advances++;
            if (fetch_iov(pgdir, uiov, xlate_iov, iov_bound, &cache_va,
                          &cache_pa, &cur) < 0)
                return -1;
            addr = (uintptr_t) GETIOVBASE(&cur);
            size = GETIOVLEN(&cur);
            if (size == 0)
                continue;
            uintptr_t last = addr + size - 1;
            if (addr > last || (!(flags & MAPADDR_FLAGS_SYSPRP) &&
                                !WITHIN_BOUNDARY(addr, last, bound)))
                return -1;
        }

        /* Kernel-direct-map addresses need no translation. */
        if (addr >= KERNEL_VIRT_BASE) {
            size_t chunk = size;
            SETIOV(&niov[nparts], (void *) addr, chunk);
            nparts++;
            bytes += (int) chunk;
            addr += chunk;
            size -= chunk;
            continue;
        }

        size_t contig = 0;
        paddr_t pa = sv39_va_to_pa(pgdir, addr, &contig);
        if (pa == (paddr_t) -1)
            return -1;

        size_t chunk = (contig < size) ? contig : size;

        SETIOV(&niov[nparts], (void *) ((uintptr_t) pa + KERNEL_VIRT_BASE),
               chunk);
        nparts++;
        bytes += (int) chunk;
        addr += chunk;
        size -= chunk;
    }

    *pnparts = nparts;
    *pparts = parts;
    *poff = (int) (GETIOVLEN(&cur) - size);
    *piov = *piov + iov_advances;

    return bytes;
}
