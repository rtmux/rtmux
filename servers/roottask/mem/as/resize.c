/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"
#include "root-process.h"


/* The page-aligned object range a resize added or removed: [lo, hi). */
struct resize_span {
    uint64_t lo;
    uint64_t hi;
};

/* The part of mapping mm that maps span, as the VAs [*startp, *endp]; 0 when
 * the mapping does not reach the span.
 */
static int span_vaddrs(const struct mm_map *mm,
                       const struct resize_span *span,
                       uintptr_t *startp,
                       uintptr_t *endp)
{
    uint64_t first = (uint64_t) mm->offset;
    uint64_t lo = max(span->lo, first);
    uint64_t hi = min(span->hi, first + (mm->end - mm->start) + 1);

    if (lo >= hi)
        return 0;
    *startp = mm->start + (uintptr_t) (lo - first);
    *endp = mm->start + (uintptr_t) (hi - 1 - first);
    return 1;
}


/* Map the pages of process pid's mappings of obp in span, with pid's address
 * space and the object locked, as mmap's setup_mappings does: a writable
 * MAP_PRIVATE mapping is privatized here, which changes pid's map list.
 * Pages past the object's current end (it may have shrunk again since the
 * growth) are marked instead (MR_TRUNC). A failure leaves the rest of a range
 * marked, so touching it is SIGBUS.
 */
static void map_grown_in(pid_t pid, path_object_t *obp, const struct resize_span *grown)
{
    root_process_t *tmp = root_proc(pid);
    struct resize_span span = *grown;
    struct mm_map *mm;

    (void) proc_wlock_adp(pid);
    proc_lock_owner_mark(pid);
    memobj_lock(obp);
    span.hi = min(span.hi, (uint64_t) ROUNDUP(obp->mem.mm.size, __PAGESIZE));
    for (mm = tmp->map.head; mm; mm = mm->next) {
        uintptr_t start;
        uintptr_t end;
        unsigned flags;

        if (!mm->obj_ref || mm->obj_ref->obp != obp || mm->obj_ref->adp != tmp->adp ||
            !span_vaddrs(mm, &span, &start, &end))
            continue;
        flags = (mm->mmap_flags & PROT_WRITE) ? MR_WRITE | MR_TRUNC : MR_TRUNC;
        /* memory_reference() may replace mm; carry on after what it left. */
        (void) memory_reference(&mm, start, end, flags, NULL);
    }
    memobj_unlock(obp);
    proc_unlock_adp(pid);
}

/* as_map_grown — after obp grew from oldsize bytes, map the newly covered pages
 * of every existing mapping of it, so that a mapping made while they were past
 * the end (marked by memory_reference with MR_TRUNC, or by a shrink) reaches
 * them without a remap, as POSIX expects once the object covers the page.
 *
 * as_resize cannot do it: it runs with the object locked, and an address space
 * is locked before an object (mmap's order), so the mapping processes are
 * collected under the object lock and then visited one at a time, each with
 * its own address-space lock taken first. Called by vm_grow, which holds
 * neither the object nor any address-space lock.
 */
static void as_map_grown(path_object_t *obp, uint64_t oldsize)
{
    struct resize_span grown;
    struct mm_object_ref * or ;
    pid_t *pids;
    unsigned n = 0;
    unsigned cap = 0;

    memobj_lock(obp);
    grown.lo = (uint64_t) ROUNDUP(oldsize, __PAGESIZE);
    grown.hi = (uint64_t) ROUNDUP(obp->mem.mm.size, __PAGESIZE);
    if (grown.lo >= grown.hi || !(obp->mem.mm.flags & MM_MEM_HAS_BAD_PAGE) ||
        !obp->mem.mm.refs) {
        memobj_unlock(obp);
        return;
    }
    for (or = obp->mem.mm.refs; or ; or = or->next)
        cap++;
    pids = malloc(cap * sizeof *pids);
    for (or = obp->mem.mm.refs; pids && or ; or = or->next) {
        unsigned i;

        for (i = 0; i < n && pids[i] != or->pid; i++)
            ;
        if (or->pid != 0 && i == n)
            pids[n++] = or->pid;
    }
    memobj_unlock(obp);
    if (!pids)
        root_warn("as_map_grown: no memory: mappings past the old end stay SIGBUS\n");

    for (unsigned i = 0; i < n; i++)
        map_grown_in(pids[i], obp, &grown);
    free(pids);
}


/* vm_grow — grow obp to at least size bytes and map what the growth made
 * reachable in existing mappings. The size check, the old size and the resize
 * are one hold of the object lock, so a concurrent change cannot turn the call
 * into a shrink; a shrink that comes before the mappings are made is caught
 * there (map_grown_in stops at the current size). When oldp is not NULL it
 * receives the size before the call. Called with no object or address-space
 * lock held. Returns EOK or an errno from the resize.
 */
int vm_grow(path_object_t *obp, size_t size, uint64_t *oldp)
{
    uint64_t oldsize;
    int r = EOK;

    memobj_lock(obp);
    oldsize = (uint64_t) obp->mem.mm.size;
    if (size > oldsize)
        r = root_vm_ops.resize(obp, size);
    memobj_unlock(obp);
    if (oldp)
        *oldp = oldsize;
    if (r == EOK && size > oldsize)
        as_map_grown(obp, oldsize);
    return r;
}


/* The object shrank: mark the pages of this mapping that are now past its end,
 * replacing their PTEs before the memory behind them is freed, so a later
 * access is SIGBUS (POSIX mmap) and never reaches a freed page.
 */
static int shrink_ref(struct mm_object_ref * or, struct mm_map *mm, void *d)
{
    uintptr_t start;
    uintptr_t end;

    if (!span_vaddrs(mm, d, &start, &end))
        return EOK;
    if (pte_bad(or->adp, start, end) != EOK) {
        /* No table for the mark: drop the PTEs instead (SIGSEGV). */
        (void) pte_unmap(or->adp, start, end, or->obp);
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
    struct resize_span span;
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
        /* The mappings of the new pages are made by as_map_grown, once the
         * object lock is dropped.
         */
    } else if (newsize < oldsize) {
        /* shrinking... */
        span.lo = newsize;
        span.hi = oldsize;
        memref_walk(obp, shrink_ref, &span);
        obp->mem.mm.flags |= MM_MEM_HAS_BAD_PAGE;
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
