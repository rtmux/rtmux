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
 * MAP_PRIVATE mapping is privatized here, which changes pid's map list. Pages
 * past the object's current end (it may have shrunk again since the growth) are
 * marked instead (MR_TRUNC). A failure leaves the rest of a range marked, so
 * touching it is SIGBUS.
 */
static void map_grown_in(pid_t pid,
                         path_object_t *obp,
                         const struct resize_span *grown)
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

        if (!mm->obj_ref || mm->obj_ref->obp != obp ||
            mm->obj_ref->adp != tmp->adp ||
            !span_vaddrs(mm, &span, &start, &end))
            continue;
        flags = (mm->mmap_flags & PROT_WRITE) ? MR_WRITE | MR_TRUNC : MR_TRUNC;
        /* memory_reference() may replace mm; carry on after what it left. */
        (void) memory_reference(&mm, start, end, flags, NULL);
    }
    memobj_unlock(obp);
    proc_unlock_adp(pid);
}

/* Objects with a growth whose mappings are not made yet (MM_MEM_GROW_PENDING):
 * as_resize queues one, under its object lock, and vm_grow_drain makes the
 * mappings once the caller holds no lock. Each queue entry holds a clone of the
 * object, which the drain drops, and counts in mm.grow_busy until its mappings
 * are made, so that the thread that grew an object can wait for them whichever
 * thread makes them. grow_mutex guards the queue and grow_busy, and is held
 * only to change or wait on them, with no other lock taken under it.
 */
static pthread_mutex_t grow_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t grow_done = PTHREAD_COND_INITIALIZER;
static path_object_t *grow_queue;

/* Record that obp grew from oldsize bytes (page aligned) and needs its existing
 * mappings made over the new pages. Called by as_resize with the object locked;
 * only an object some mapping marked past its end needs it.
 */
static void grow_pending(path_object_t *obp, uint64_t oldsize)
{
    if (!(obp->mem.mm.flags & MM_MEM_HAS_BAD_PAGE) || !obp->mem.mm.refs)
        return;
    if (obp->mem.mm.flags & MM_MEM_GROW_PENDING) {
        obp->mem.mm.grow_from = min(obp->mem.mm.grow_from, (off64_t) oldsize);
        return;
    }
    obp->mem.mm.flags |= MM_MEM_GROW_PENDING;
    obp->mem.mm.grow_from = (off64_t) oldsize;
    (void) path_object_clone(obp);
    pthread_mutex_lock(&grow_mutex);
    obp->mem.mm.grow_next = grow_queue;
    __atomic_add_fetch(&obp->mem.mm.grow_busy, 1, __ATOMIC_RELAXED);
    __atomic_store_n(&grow_queue, obp, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&grow_mutex);
}

/* The lowest pid above after that has a mapping of obp, or 0 when none has. */
static pid_t next_mapper(path_object_t *obp, pid_t after)
{
    struct mm_object_ref * or ;
    pid_t next = 0;

    for (or = obp->mem.mm.refs; or ; or = or->next) {
        if (or->pid > after && (!next || or->pid < next))
            next = or->pid;
    }
    return next;
}

/* Map the pending growth of one object into every existing mapping of it, so
 * that a mapping made while those pages were past the end (marked by
 * memory_reference with MR_TRUNC, or by a shrink) reaches them without a remap,
 * as POSIX expects once the object covers the page. The mapping processes are
 * visited one at a time in pid order, each found under the object lock and then
 * mapped with its own address-space lock taken first (mmap's order); the order
 * makes the walk end however the mappings change meanwhile, and it needs no
 * memory.
 */
static void map_grown(path_object_t *obp)
{
    struct resize_span grown;
    pid_t pid = 0;

    memobj_lock(obp);
    obp->mem.mm.flags &= ~(unsigned) MM_MEM_GROW_PENDING;
    grown.lo = (uint64_t) obp->mem.mm.grow_from;
    grown.hi = (uint64_t) ROUNDUP(obp->mem.mm.size, __PAGESIZE);
    while (grown.lo < grown.hi && (pid = next_mapper(obp, pid))) {
        memobj_unlock(obp);
        map_grown_in(pid, obp, &grown);
        memobj_lock(obp);
    }
    memobj_unlock(obp);
}

/* vm_grow_drain — make the mappings of every pending growth (see grow_pending).
 * When grown is not NULL, also wait until grown has no growth left whose
 * mappings another drain is still making, so the caller that grew it can answer
 * its client knowing every mapping reaches the new pages. The caller holds no
 * object or address-space lock; every path that resizes with such a lock held
 * drains once it has dropped them, and keeps a reference on grown.
 */
void vm_grow_drain(path_object_t *grown)
{
    /* Most calls find nothing to do: skip the mutex then. A growth queued after
     * this check is drained by the thread that queued it.
     */
    if (!__atomic_load_n(&grow_queue, __ATOMIC_ACQUIRE) &&
        (!grown ||
         !__atomic_load_n(&grown->mem.mm.grow_busy, __ATOMIC_ACQUIRE)))
        return;
    pthread_mutex_lock(&grow_mutex);
    for (;;) {
        path_object_t *obp = grow_queue;

        if (!obp) {
            if (!grown || !grown->mem.mm.grow_busy)
                break;
            pthread_cond_wait(&grow_done, &grow_mutex);
            continue;
        }
        __atomic_store_n(&grow_queue, obp->mem.mm.grow_next, __ATOMIC_RELAXED);
        pthread_mutex_unlock(&grow_mutex);
        map_grown(obp);
        pthread_mutex_lock(&grow_mutex);
        if (__atomic_sub_fetch(&obp->mem.mm.grow_busy, 1, __ATOMIC_RELEASE) ==
            0)
            pthread_cond_broadcast(&grow_done);
        pthread_mutex_unlock(&grow_mutex);
        path_object_done(obp);
        pthread_mutex_lock(&grow_mutex);
    }
    pthread_mutex_unlock(&grow_mutex);
}


/* vm_grow — grow obp to at least size bytes and map what the growth made
 * reachable in existing mappings. The size check, the resize and the record in
 * g are one hold of the object lock, so a concurrent change cannot turn the
 * call into a shrink. When g is not NULL it receives the size before the call
 * (from), the size it left (to), and the object's write generation, which every
 * call bumps (gen). Called with no object or address-space lock held.
 *
 * Returns EOK or an errno from the resize.
 */
int vm_grow(path_object_t *obp, size_t size, struct vm_growth *g)
{
    uint64_t oldsize;
    int r = EOK;

    memobj_lock(obp);
    oldsize = (uint64_t) obp->mem.mm.size;
    if (size > oldsize)
        r = root_vm_ops.resize(obp, size);
    ++obp->mem.mm.wgen; /* a write that does not grow is still a writer */
    if (g) {
        g->from = oldsize;
        g->to = (uint64_t) obp->mem.mm.size;
        g->gen = obp->mem.mm.wgen;
    }
    memobj_unlock(obp);

    /* Also when this call did not grow it: another growth may still be having
     * its mappings made, and the caller answers a client next.
     */
    vm_grow_drain(obp);
    return r;
}


/* vm_undo_grow — a write that grew obp (vm_grow, g) failed: take the growth
 * back, unless anything since could have put bytes in the grown range. The
 * write generation moves on with every write and size change, and with every
 * page a process other than roottask gets mapped (memory_reference): a store
 * through a mapping leaves no other trace, so a mapping made over the new
 * pages, by the growth's own drain or a later mmap, keeps the growth.
 */
void vm_undo_grow(path_object_t *obp, const struct vm_growth *g)
{
    memobj_lock(obp);
    if ((uint64_t) obp->mem.mm.size == g->to && obp->mem.mm.wgen == g->gen)
        (void) root_vm_ops.resize(obp, (size_t) g->from);
    memobj_unlock(obp);
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

/* memobj_pmem_walk callback for zero_tail: zero the quantum from *d's offset
 * within it to its end, if the quantum has been written. The zeroing goes
 * through kernel_page_zero on the physical page, so it needs no mapping and no
 * address-space lock and runs under the object lock.
 */
static int do_zero(path_object_t *obp,
                   off64_t off,
                   struct pa_quantum *pq,
                   size_t num,
                   void *d)
{
    size_t pg_off = *(size_t *) d & (QUANTUM_SIZE - 1);

    (void) obp;
    (void) off;
    (void) num;
    if (pa_pq_flags(pq) & PAQ_FLAG_MODIFIED)
        return kernel_page_zero(pa_quantum_to_paddr(pq) + pg_off,
                                QUANTUM_SIZE - pg_off);
    return EOK;
}


/* Zero the part of the last page past size, so that a later growth over it
 * reads zeros (a shrink to a size inside a page keeps that page). Called with
 * the object locked.
 */
static int zero_tail(path_object_t *obp, size_t size)
{
    off64_t pg = (off64_t) (size & ~(size_t) (QUANTUM_SIZE - 1));

    return memobj_pmem_walk(MPW_SYSRAM, obp, pg, pg, do_zero, &size);
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
    if (!has_lock)
        memobj_lock(obp);
    if (((size & (QUANTUM_SIZE - 1)) != 0) &&
        ((off64_t) size < obp->mem.mm.size)) {
        /* non quantum aligned new size and we're shrinking. We need to zero the
         * piece of the last page (if it's already been initialized).
         */
        r = zero_tail(obp, size);
        if (r != EOK)
            goto fail2;
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

        /* The mappings of the new pages are made by vm_grow_drain, once the
         * caller has dropped its locks.
         */
        if (r == EOK)
            grow_pending(obp, oldsize);
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
    obp->mem.mm
        .wgen++; /* any size change voids a pending undo (vm_undo_grow) */
    if (!has_lock)
        memobj_unlock(obp);
    return EOK;

fail2:
    if (!has_lock)
        memobj_unlock(obp);
    return r;
}
