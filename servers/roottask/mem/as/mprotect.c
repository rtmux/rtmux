/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"


int as_mprotect(pid_t pid,
                process_t *prp,
                uintptr_t vaddr,
                size_t len,
                int prot)
{
    address_t *as;
    int r;
    struct map_set ms;
    struct mm_map *mm;
    unsigned old;
    unsigned cache_flags;
    struct mm_object_ref * or ;
    path_object_t *obp;
    uintptr_t end_vaddr;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;

    if (!tmp)
        return EINVAL;
    as = tmp->adp;
    r = map_isolate(&ms, &tmp->map, vaddr, len, MI_SPLIT);
    if (r != EOK)
        goto fail1;

    /* START KLUDGE We need to check the vaddr for being page aligned, but only
     * if MAP_ELF isn't on. See vm-ctrl.c where it calls root_vm_ops.mprotect
     * for details.
     */
    if (!(ms.first->mmap_flags & MAP_ELF) &&
        (mm_flags & MM_FLAG_ENFORCE_ALIGNMENT)) {
        if (ADDR_OFFSET(vaddr) != 0) {
            r = EINVAL;
            goto fail2;
        }
    }
    /* END KLUDGE */

    proc_lock_owner_mark(pid);

    end_vaddr = ms.last->end;
    mm = ms.first;
    for (;;) {
        if ((mm->extra_flags & EXTRA_FLAG_RDONLY) && (prot & PROT_WRITE)) {
            r = EACCES;
            goto fail2;
        }
        or = mm->obj_ref;
        if (or) {
            obp = or->obp;
            old = mm->mmap_flags & PROT_MASK;
            if (old != prot) {
                mm->mmap_flags =
                    (mm->mmap_flags & ~(unsigned) PROT_MASK) | (unsigned) prot;

                /* Permission-only change: use pte_prot() which issues
                 * PTE_OP_PROT — preserves the existing physical address in each
                 * PTE and only updates permission bits.
                 *
                 * The old code used memory_reference() → pte_map() which issues
                 * PTE_OP_MAP and overwrites the PTE with whatever paddr happens
                 * to be in the walk data (stale/wrong), corrupting user page
                 * mappings.
                 */
                r = pte_prot(as, mm->start, mm->end, mm->mmap_flags, obp);
                if (r != EOK)
                    goto fail2;
                /* RUSH3: Can these be moved into memory_reference()? */
                cache_flags = 0;
                if (!(old & PROT_NOCACHE) && (prot & PROT_NOCACHE)) {
                    cache_flags |= MS_SYNC | MS_INVALIDATE;
                    if (old & PROT_EXEC) {
                        cache_flags |= MS_INVALIDATE_ICACHE;
                    }
                }
                if ((old & PROT_WRITE) && (prot & PROT_EXEC)) {
                    cache_flags |= MS_INVALIDATE_ICACHE;
                }

                /* Writable->executable transition: this is how every loader
                 * finalizes freshly-written code (ELF loader and the dynamic
                 * linker both map a segment RWX, read the code in, then
                 * mprotect it down to R-X here). The bytes were written via
                 * ordinary data stores, so before the code can be fetched on
                 * ANY hart, fence.i must run there — otherwise a hart holding
                 * the recycled page's previous tenant's I-cache lines executes
                 * stale instructions (the residual FU740 fault: sepc lands on a
                 * non-store instruction yet takes a store page-fault).
                 * Broadcast fence.i to every hart now. See
                 * kernel_icache_sync_all / cpu_icache_sync_all.
                 */
                if (cache_flags & MS_INVALIDATE_ICACHE) {
                    (void) kernel_icache_sync_all();
                }
            }
        }
        if (mm->end >= end_vaddr)
            break;
        mm = mm->next;
    }
fail2:
    map_coalese(&ms);
fail1:
    return r;
}
