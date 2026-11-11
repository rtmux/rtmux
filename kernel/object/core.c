/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>


/* The object allocator for the system. All kernel objects are a fixed size and
 * allocated using this routine. There is a soul list for each type of object.
 * When an object is released we place it back on the soul list where it can be
 * quickly reused by the next allocation for this object type. This is much
 * faster than calling malloc().
 *
 * If objects have a special alignment constraint, we allocate "align" extra,
 * and return an aligned pointer as the object.
 */

#define ALIGN_PTR(ptr, align) \
    (void **) (((uintptr_t) (ptr) + (align)) & ~((uintptr_t) (align) - 1))
#define SAVED_PTR(ptr) (*(void **) ((uintptr_t) ptr - sizeof(void *)))

/* Assert object_alloc/object_free are called from a context that may touch the
 * soul freelist: a proc thread, a locked kernel, or an unnested interrupt
 * (uniprocessor only). Debug-only.
 */
static inline void object_alloc_context_check(void)
{
#ifndef NDEBUG
    {
        unsigned ik;

#if CONFIG_PROCESSORS_MAX > 1
        interrupt_disable();
        ik = get_inkernel();
        if ((ik & INKERNEL_NOW) && !(ik & INKERNEL_LOCK) &&
            ((ik >> 24) == get_cpunum())) {
            crash();
        }
        if (pcpu_array[0].pc_alive) {
            interrupt_enable();
        }
#else
        ik = get_inkernel();
        if ((ik != 0) && !(ik & INKERNEL_LOCK) && (ik != 1)) {
            crash();
        }
#endif
    }
#endif
}

/* Shared soul allocator body. See object_alloc / object_alloc_reserved.
 *
 *   reserved_floor : keep (total - used) above this after the pop. Callers pass
 *                    soulp->reserved to protect the realtime reserve, or 0 to
 *                    draw the reserve down.
 *   may_grow       : grow the list from the heap when it cannot satisfy the pop
 *                    without crossing the floor. Realtime delivery passes false
 *                    (the heap layer is unsafe from the fire path).
 *   bypass_limit   : skip the owning process's per-type ceiling (still bills
 *                    lip->cur so object_free stays balanced).
 */
static void *object_alloc_common(process_t *prp,
                                 soul_t *soulp,
                                 uint32_t reserved_floor,
                                 bool may_grow,
                                 bool bypass_limit)
{
    void **ptr;
    limits_t *lip;
    int type = soulp->type;

    object_alloc_context_check();

    lip = prp ? prp->limits : NULL;

    for (;;) {
        unsigned long iflags = kspinlock_lock_irqsave(&soul_slock);

        if (!bypass_limit && lip && lip->cur[type] >= lip->max[type] &&
            (prp->flags & RTMUX_FLG_PROC_NO_LIMITS) == 0) {
            kspinlock_unlock_irqrestore(&soul_slock, iflags);
            return NULL;
        }

        /* Non-reserved callers keep the realtime reserve intact: only pop when
         * strictly more than reserved_floor souls are free, otherwise grow.
         */
        if ((soulp->total - soulp->used) > reserved_floor &&
            (ptr = soulp->next)) {
            soulp->next = *ptr;
            *ptr = NULL;
            if (lip)
                ++lip->cur[type];

            /* Track highwater. */
            if (soulp->highwater <= ++soulp->used)
                soulp->highwater = soulp->used;

            /* Every 100 allocations, promote the observed highwater to the new
             * minimum retained count.
             */
            if (++soulp->counter >= 100) {
                soulp->newmin = soulp->highwater;
                soulp->highwater = soulp->min;
                soulp->counter = 0;
            }
            kspinlock_unlock_irqrestore(&soul_slock, iflags);
            break;
        }

        kspinlock_unlock_irqrestore(&soul_slock, iflags);
        if (!may_grow || !object_grow(soulp))
            return NULL;
    }

    if (soulp->align) {
        void **aligned_ptr;

        aligned_ptr = ALIGN_PTR(ptr, soulp->align);
        SAVED_PTR(aligned_ptr) = ptr;
#ifndef NDEBUG
        if ((uintptr_t) aligned_ptr & (soulp->align - 1))
            crash();
#endif
        return aligned_ptr;
    }
    return ptr;
}

/* Allocate a fixed-size kernel object from a soul list.
 *
 * Enforces the owning process's per-type limit, pops a soul (growing the list
 * if empty), tracks highwater statistics, and returns an aligned pointer when
 * the soul list has an alignment constraint. Keeps soulp->reserved souls back
 * for realtime delivery. Parameters:
 *   prp   : owning process for limit accounting (may be NULL)
 *   soulp : soul list to allocate from
 * Return value: pointer to the object, or NULL on limit/allocation failure.
 */
void *rdecl object_alloc(process_t *prp, soul_t *soulp)
{
#ifdef CONFIG_DEBUG_INJECT
    /* Ordinary allocation only: object_alloc_reserved never fails this way. */
    if (inject_should_fail_alloc(prp, soulp))
        return NULL;
#endif
    return object_alloc_common(prp, soulp, soulp->reserved, true, false);
}

/* Allocate from the realtime reserve, allocation-free.
 *
 * For IRQ/timer SIGEV_NOTIFICATION delivery running from the fire path: draws
 * the reserve down (reserved_floor 0), never grows the heap, and bypasses the
 * per-process ceiling (the source paid for the slot at object_reserve time).
 * Succeeds without dynamic allocation as long as the matching object_reserve
 * grew the pool, which is the allocation-free-delivery guarantee. Parameters:
 *   prp   : owning process for limit accounting (may be NULL)
 *   soulp : soul list to allocate from
 * Return value: pointer to the object, or NULL if the reserve is exhausted.
 */
void *rdecl object_alloc_reserved(process_t *prp, soul_t *soulp)
{
    return object_alloc_common(prp, soulp, 0, false, true);
}

/* Grow the reserved floor by one, first ensuring one extra free soul exists.
 *
 * Called at attach/create of a SIGEV_NOTIFICATION IRQ/timer source so a later
 * object_alloc_reserved on the fire path cannot fail. Grows the pool (heap
 * touch is safe here, this is kcall context) until a free soul is available
 * above the current reserve, then claims it. Parameters:
 *   soulp : soul list to reserve capacity in
 * Return value: true on success, false if the heap could not grow the pool.
 */
bool rdecl object_reserve(soul_t *soulp)
{
    for (;;) {
        unsigned long iflags = kspinlock_lock_irqsave(&soul_slock);

        if ((soulp->total - soulp->used) > soulp->reserved) {
            ++soulp->reserved;
            kspinlock_unlock_irqrestore(&soul_slock, iflags);
            return true;
        }
        kspinlock_unlock_irqrestore(&soul_slock, iflags);
        if (!object_grow(soulp))
            return false;
    }
}

/* Release one reserved-floor slot claimed by object_reserve. */
void rdecl object_unreserve(soul_t *soulp)
{
    unsigned long iflags = kspinlock_lock_irqsave(&soul_slock);

    if (soulp->reserved)
        --soulp->reserved;
    kspinlock_unlock_irqrestore(&soul_slock, iflags);
}


/* Return an object to its soul list.
 *
 * Recovers the unaligned base for aligned souls, updates limit accounting, and
 * either frees the soul back to the heap (when the list is over its minimum or
 * the soul came from the critical pool) or zeroes and re-links it onto the free
 * list for reuse. Parameters:
 *   prp   : owning process for limit accounting (may be NULL)
 *   soulp : soul list the object belongs to
 *   ptr   : object to free
 */
static void object_free_track_heap_common(process_t *prp,
                                          soul_t *soulp,
                                          void *ptr,
                                          bool *heap_returned,
                                          uint64_t *cache_clear_cycles,
                                          uint64_t *cache_link_cycles)
{
    limits_t *lip;
    uint64_t t0, t1, t2;
    bool skip_clear;
    unsigned long iflags;

    if (heap_returned)
        *heap_returned = false;
    if (cache_clear_cycles)
        *cache_clear_cycles = 0;
    if (cache_link_cycles)
        *cache_link_cycles = 0;

    /* used to check to make sure we were in the kernel but now we can't do that
     * since proc may call these routines in its own context
     */

    if (soulp->align) {
        ptr = SAVED_PTR(ptr);
    }

    /* Serialize accounting, the heap return, and the freelist push under
     * soul_slock. _sfree / crit_sfree take alloc_slock (MEMPART), nested
     * correctly under soul_slock; the heap-free path never re-enters the soul
     * allocator, so there is no soul_slock recursion. irqsave to match the
     * alloc path (frees can also run from trap context).
     *
     * One global lock across every soul type is the simplest correct fix. Its
     * ceiling is that a heap-returning free holds soul_slock across the _sfree
     * heap scan, lengthening the shared critical section; if that shows up in
     * realtime latency budgets, split into per-type soul locks or push to the
     * freelist before returning surplus souls to the heap out of band.
     */
    iflags = kspinlock_lock_irqsave(&soul_slock);

    if (prp && (lip = prp->limits)) {
        if (--lip->cur[soulp->type] == ~0U) {
            crash();
        }
    }

    --soulp->used;
    skip_clear = (soulp->flags & SOUL_NOCLEAR) &&
                 (soulp != &thread_souls ||
                  TYPE_MASK(((thread_t *) ptr)->type) == TYPE_THREAD);

    /* Never shrink the pool below the realtime reserve. Returning this soul to
     * the heap drops total by one; only do so while (total - used) stays above
     * reserved, so object_alloc_reserved keeps a free soul to draw from. used
     * is already decremented, so the post-return free count is total-1-used.
     */
    if (!(soulp->flags & SOUL_NOFREE) &&
        (soulp->total - 1 - soulp->used) >= soulp->reserved) {
        /* Do we have too many souls on this list? */
        if ((soulp->total > soulp->newmin)) {
            /* Yes. Free the soul back to the system. */
            _sfree(ptr, soulp->size + soulp->align);
            --soulp->total;
            kspinlock_unlock_irqrestore(&soul_slock, iflags);
            if (heap_returned)
                *heap_returned = true;
            return;
        }
        /* Was it a critical allocation? */
        if (crit_sfree(ptr, soulp->size + soulp->align)) {
            --soulp->total;
            kspinlock_unlock_irqrestore(&soul_slock, iflags);
            if (heap_returned)
                *heap_returned = true;
            return;
        }
    }
    /* Nope. Return this soul to the list. */
    if (cache_clear_cycles || cache_link_cycles) {
        if (skip_clear) {
            t0 = t1 = cpu_clock_ticks();
        } else {
            t0 = cpu_clock_ticks();
            memset(ptr, 0, soulp->size + soulp->align);
            t1 = cpu_clock_ticks();
        }
        *(void **) ptr = soulp->next;
        soulp->next = ptr;
        t2 = cpu_clock_ticks();
        if (cache_clear_cycles)
            *cache_clear_cycles = skip_clear ? 0 : t1 - t0;
        if (cache_link_cycles)
            *cache_link_cycles = t2 - t1;
    } else {
        if (!skip_clear)
            memset(ptr, 0, soulp->size + soulp->align);
        *(void **) ptr = soulp->next;
        soulp->next = ptr;
    }
    kspinlock_unlock_irqrestore(&soul_slock, iflags);
}

void rdecl object_free_track_heap(process_t *prp,
                                  soul_t *soulp,
                                  void *ptr,
                                  bool *heap_returned)
{
    object_free_track_heap_common(prp, soulp, ptr, heap_returned, NULL, NULL);
}

void rdecl object_free_track_heap_detail(process_t *prp,
                                         soul_t *soulp,
                                         void *ptr,
                                         bool *heap_returned,
                                         uint64_t *cache_clear_cycles,
                                         uint64_t *cache_link_cycles)
{
    object_free_track_heap_common(prp, soulp, ptr, heap_returned,
                                  cache_clear_cycles, cache_link_cycles);
}

void rdecl object_free(process_t *prp, soul_t *soulp, void *ptr)
{
    object_free_track_heap(prp, soulp, ptr, NULL);
}



/* Grow a soul list by allocating new souls from the heap.
 *
 * On first growth, seeds the list with the configured minimum count. Honours
 * the per-CPU critical-allocation override when sizing the backing allocation.
 * Parameters:
 *   soulp : soul list to grow
 * Return value: true if a soul was added to the free list, false on allocation
 *               failure. Success only means the list was grown: another CPU may
 *               pop the new soul before this caller retries, which is why no
 *               pointer is handed back.
 */
bool rdecl object_grow(soul_t *soulp)
{
    void **ptr;
    unsigned num;

    /* used to check to make sure we were in the kernel but now we can't do that
     * since proc may call these routines in its own context
     */

    /* First growth seeds the list to its configured minimum. Decide and record
     * the seed under soul_slock so two CPUs cannot both seed, then run the
     * recursive seeding growths with the lock released: the recursion and the
     * _scalloc below must not hold soul_slock.
     */
    {
        unsigned long iflags = kspinlock_lock_irqsave(&soul_slock);
        bool seed = (soulp->newmin == 0);

        if (seed)
            soulp->newmin = soulp->min;
        kspinlock_unlock_irqrestore(&soul_slock, iflags);

        /* min - 1 recursive growths; the caller's own growth below makes up the
         * min'th. Counting down to 1 rather than pre-decrementing keeps a soul
         * declared with min 0 from wrapping the unsigned counter.
         */
        if (seed) {
            for (num = soulp->min; num > 1; num--) {
                (void) object_grow(soulp);
            }
        }
    }

    /* Per-CPU override expresses call-time critical-allocation intent.
     * NONCRITICAL forces "not critical" regardless of soul flags; CRITICAL
     * forces "critical"; NONE falls back to the soul's own SOUL_CRITICAL bit.
     * See kernel/include/lock.h.
     */
    {
        uint8_t override = pcpu_array[get_cpunum()].pc_soul_alloc_override;
        bool critical = (soulp->flags & SOUL_CRITICAL) != 0;
        if (override == SOUL_OVERRIDE_NONCRITICAL)
            critical = false;
        else if (override == SOUL_OVERRIDE_CRITICAL)
            critical = true;
        if (critical)
            alloc_critical++;
        ptr = _scalloc(soulp->size + soulp->align);
        if (critical)
            alloc_critical--;
    }

    if (!ptr)
        return false;

    {
        unsigned long iflags = kspinlock_lock_irqsave(&soul_slock);

        ++soulp->total;
        *ptr = soulp->next;
        soulp->next = ptr;
        kspinlock_unlock_irqrestore(&soul_slock, iflags);
    }

    return true;
}



/* Reserve extra per-object data at the end of a soul.
 *
 * Grows the soul size by "size" bytes (after rounding the current size up for
 * alignment) and returns the offset of the reserved region. Must be called
 * before any soul of this type is allocated. Parameters:
 *   soulp : soul list to extend
 *   size  : number of extra bytes to reserve
 * Return value: byte offset of the reserved region within the soul.
 */
unsigned rdecl object_register_data(soul_t *soulp, size_t size)
{
    unsigned r;

    CRASHCHECK(soulp->next);

    r = (unsigned) ROUNDUP(soulp->size, sizeof(uint64_t));
    soulp->size = (uint32_t) (r + size);
    return r;
}
