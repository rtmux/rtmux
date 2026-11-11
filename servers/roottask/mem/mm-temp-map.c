/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"

/* Temporarily map a run of physical quanta and invoke a callback on it.
 *
 * When the run lies in the kernel 1:1 direct map and needs no special handling,
 * calls <func> directly on the direct-map VA. Otherwise carves out a temporary
 * virtual range in the process as, maps the run there in pieces, invokes <func>
 * for each piece, and unmaps.
 *
 * NOTE: this function may be overridden with CPU-specific code on some systems
 * (e.g. ARM). Parameters:
 *   adp     : address space to map into
 *   user_va : user VA the run corresponds to (used to pick a temp range)
 *   pq      : first quantum of the run
 *   mm      : the mapping whose flags govern protection/caching
 *   len     : length of the run in bytes
 *   func    : callback invoked per mapped piece (VA, length, d)
 *   d       : opaque argument passed to <func>
 * Return value: EOK on success, or the errno from pte_map / <func>.
 */
int pte_temp_map(address_t *adp,
                 uintptr_t user_va,
                 struct pa_quantum *pq,
                 struct mm_map *mm,
                 size_t len,
                 int (*func)(void *, size_t, void *),
                 void *d)
{
    paddr_t paddr;
    uintptr_t end;
    int r;
    size_t piece;
    struct mm_object_ref * or ;
    path_object_t *obp;
    uintptr_t vaddr;
    uintptr_t tmap_start;
    uintptr_t tmap_end;
    uintptr_t use_start;
    uintptr_t use_end;
    unsigned min_len;

    paddr = pa_quantum_to_paddr(pq);
    vaddr = (uintptr_t) paddr + CPU_1TO1_VADDR_BIAS;

    or = mm->obj_ref;
    obp = or->obp;

    if (CPU_1TO1_IS_PADDR(paddr + len) && !(mm->mmap_flags & PROT_NOCACHE) &&
        (((obp->hdr.type != OBJECT_MEM_SHARED) &&
          (obp->hdr.type != OBJECT_MEM_TYPED)) ||
         !(obp->mem.mm.flags & MM_SHMEM_SPECIAL))) {
        root_trace(
            "pte_temp_map: 1to1 paddr=%lx vaddr=%lx len=%lx type=%d uva=%lx\n",
            (unsigned long) paddr, (unsigned long) vaddr, (unsigned long) len,
            obp->hdr.type, (unsigned long) user_va);
        return func((void *) vaddr, len, d);
    }

    piece = len;
#define MAX_TEMP_MAPPING (64 * 1024)
    if (piece > MAX_TEMP_MAPPING)
        piece = MAX_TEMP_MAPPING;
    min_len = __PAGESIZE;
    if (piece < min_len)
        piece = min_len;
    /* Check that we can use the tmp area and does not overlap */
    if (piece <= adp->tmap_size &&
        ((adp->tmap_base >= (user_va + len)) ||
         ((adp->tmap_base + adp->tmap_size) <= user_va))) {
        tmap_start = adp->tmap_base;
        piece = adp->tmap_size;
    } else {
        for (;;) {
            /* We don't have to actually allocate the virtual address since we
             * have the as write-locked - nobody else can get in here to fiddle
             * things.
             */
            tmap_start = map_find_va(&adp->map, user_va, piece, 0, 0);
            if (tmap_start != VA_INVALID)
                break;
            piece >>= 1;
            if (piece < min_len) {
                /* When we created the as, we snipped off some pages at the end
                 * of the legal user address space range to use as a last chance
                 * temp mapping area (see mcreate.c). We only use them as a last
                 * resort because:
                 * 1. It's minimum size - we can be more efficient if we can
                 *    do the whole temp mapping range in one go.
                 * 2. Since the vaddr is off in the nether regions, we'd likely
                 *    have to allocate an L2 page table entry to cover it that
                 *    we normally wouldn't have. By originally trying vaddrs
                 *    in the area of the actual user address first, we're likely
                 *    to be able to share an L2 with one we're using anyway.
                 */
                piece = min_len;
                tmap_start = adp->map.end + 1;
                break;
            }

            /* We might have been passed in a non power-of-two number of pages.
             */
            piece = ROUNDUP(piece, __PAGESIZE);
        }
        adp->tmap_base = tmap_start;
        adp->tmap_size = piece;
    }
    tmap_end = tmap_start + piece - 1;
    use_start = tmap_end;
    use_end = tmap_start;

    for (;;) {
        vaddr = tmap_start;
        piece = (tmap_end - vaddr) + 1;
        if (len < piece)
            piece = len;
        end = vaddr + piece - 1;
        r = pte_map(adp, vaddr, end,
                    (int) (mm->mmap_flags | (PROT_READ | PROT_WRITE)), obp,
                    paddr, PTE_OP_TEMP);
        if (r != EOK)
            break;
        if (vaddr < use_start)
            use_start = vaddr;
        if (end > use_end)
            use_end = end;
        r = func((void *) vaddr, piece, d);
        if (r != EOK)
            break;
        user_va += piece;
        paddr += piece;
        len -= piece;
        if (len == 0)
            break;
    }
    if (use_start < use_end) { /* in case pte_map() failed on first iteration */
        pte_unmap(adp, use_start, use_end, obp);
    }
    return r;
}
