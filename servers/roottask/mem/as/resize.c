/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"


static int clear_bad_pages(struct mm_object_ref * or,
                           struct mm_map *mm,
                           void *d)
{
    size_t oldsize = *(uint64_t *) d;
    size_t end;
    size_t grow;
    size_t map_size;

    map_size = (mm->end - mm->start) + 1;
    end = (size_t) mm->offset + map_size;
    if (end > oldsize) {
        grow = end - oldsize;
        if (grow > map_size) {
            grow = map_size;
        }

        /* Turn off any 'badness' indicator in the PTE that might have been set
         * by pte_bad().
         */
        pte_unmap(or->adp, (mm->end - grow) + 1, mm->end, or->obp);
    }
    return EOK;
}


static int shrink_ref(struct mm_object_ref * or, struct mm_map *mm, void *d)
{
    size_t newsize = *(uint64_t *) d;
    size_t end;
    size_t shrink;
    size_t map_size;

    map_size = (mm->end - mm->start) + 1;
    end = (size_t) mm->offset + map_size;
    if (end > newsize) {
        shrink = end - newsize;
        if (shrink > map_size) {
            shrink = map_size;
        }
        pte_unmap(or->adp, (mm->end - shrink) + 1, mm->end, or->obp);
    }
    return EOK;
}

static int do_zero(path_object_t *obp,
                   off64_t off,
                   struct pa_quantum *pq,
                   size_t num,
                   void *d)
{
    size_t pg_off = *(size_t *) d & (QUANTUM_SIZE - 1);
    void *vaddr;
    size_t size;
    int r;

    /* If the quantum's been modified, we have to zero the portion beyond end of
     * the object so that accesses don't pick up whatever content was there
     * before.
     */
    if (pa_pq_flags(pq) & PAQ_FLAG_MODIFIED) {
        r = as_mmap(ROOTTASK_PID, roottask_prp, 0, QUANTUM_SIZE,
                    PROT_READ | PROT_WRITE, MAP_SHARED, obp, (uint64_t) off, 0,
                    QUANTUM_SIZE, NOFD, &vaddr, &size);
        if (r != EOK)
            return r;
        memset((uint8_t *) vaddr + pg_off, 0, QUANTUM_SIZE - pg_off);
        (void) as_munmap(ROOTTASK_PID, roottask_prp, (uintptr_t) vaddr, size,
                         0);
    }
    return EOK;
}


static int zero_tail(path_object_t *obp, size_t size)
{
    pid_t old;
    size_t pg_off;
    int r;

    old = process_bind(-1);
    if (old != 0)
        process_bind(0);
    process_bind(ROOTTASK_PID);
    proc_wlock_adp(ROOTTASK_PID);
    memobj_lock(obp);
    pg_off = size & ~(size_t) (QUANTUM_SIZE - 1);
    r = memobj_pmem_walk(MPW_SYSRAM, obp, (off64_t) pg_off, (off64_t) pg_off,
                         do_zero, &size);
    memobj_unlock(obp);
    proc_unlock_adp(ROOTTASK_PID);
    process_bind(old);
    return r;
}


int as_resize(path_object_t *obp, size_t size)
{
    uint64_t newsize;
    uint64_t oldsize;
    unsigned flags;
    int r;
    int has_lock;
    uintptr_t vaddr;

    flags = MAP_ANON;
    switch (obp->hdr.type) {
    case OBJECT_MEM_SHARED:
        if (obp->mem.mm.flags & MM_SHMEM_SPECIAL) {
            switch (obp->mem.mm.flags & (SHMCTL_ANON | SHMCTL_PHYS)) {
            case 0:
            case SHMCTL_ANON:
                break;
            case SHMCTL_PHYS:
                flags = 0;
                break;
            case SHMCTL_ANON | SHMCTL_PHYS:
                flags |= MAP_PHYS;
                break;
            default:
                break;
            }
            if (obp->mem.mm.flags & SHMCTL_ISADMA) {
                flags |= MAP_ANON | MAP_PHYS;
            }
            if (obp->mem.mm.flags & SHMCTL_NOX64K) {
                flags |= MM_MAP_NOX64K;
            }
        }
        vaddr = obp->shmem.vaddr;
        break;
    case OBJECT_MEM_FD:
        vaddr = VA_INVALID;
        break;

        /* RUSH2: Explicitly listed here so we can find it if we decide to
         * RUSH2: make OBJECT_MEM_TYPED an OBJECT_MEM_SHARED with a MM_MEM_TYPED
         * RUSH2: flag bit. In that case we'll have to add gear to the
         * OBJECT_MEM_SHARED RUSH2: case above
         */
    case OBJECT_MEM_TYPED:
        /* fall through */
    default:
        return EINVAL;
    }

    has_lock = proc_mux_haslock(&obp->mem.mm.mux, 0);
    if (!has_lock) {
        if (((size & (QUANTUM_SIZE - 1)) != 0) && (size < obp->mem.mm.size)) {
            /* non quantum aligned new size and we're shrinking. We need to zero
             * the piece of the last page (if it's already been initialized).
             */
            r = zero_tail(obp, size);
            if (r != EOK)
                goto fail1;
        }
        memobj_lock(obp);
    }

    oldsize = (uint64_t) ROUNDUP(obp->mem.mm.size, __PAGESIZE);
    newsize = ROUNDUP(size, __PAGESIZE);

    if (newsize > oldsize) {
        /* growing... */
        if (vaddr != VA_INVALID) {
            /* Can't grow a global object after it's been assigned an address */
            r = ENOMEM;
            goto fail2;
        }
        r = memobj_offset_check(obp, 0, newsize);
        if (r == EOK) {
            r = memobj_pmem_add(obp, (off64_t) oldsize, newsize - oldsize,
                                (int) flags);
        }
        if (obp->mem.mm.flags & MM_MEM_HAS_BAD_PAGE) {
            memref_walk(obp, clear_bad_pages, &oldsize);
            obp->mem.mm.flags &= ~(unsigned) MM_MEM_HAS_BAD_PAGE;
        }
    } else if (newsize < oldsize) {
        /* shrinking... */
        memref_walk(obp, shrink_ref, &newsize);
        r = memobj_pmem_del_end(obp, (off64_t) newsize, ~(off64_t) 0);
    } else {
        r = EOK;
    }
    if ((newsize == 0) && (obp->mem.mm.pmem))
        crash();
    if (r != EOK)
        goto fail2;

    obp->mem.mm.size = (off64_t) size;
    if (!has_lock)
        memobj_unlock(obp);
    return EOK;

fail2:
    if (!has_lock)
        memobj_unlock(obp);

fail1:
    return r;
}
