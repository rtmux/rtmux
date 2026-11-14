/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <core/globals.h>

#define ALIGN(x) \
    ((size_t) ((((size_t) (x)) + (_MALLOC_ALIGN - 1)) & ~(_MALLOC_ALIGN - 1)))
#define PTRDIFF(x, y) (((paddr_t) (x)) - ((paddr_t) (y)))
#define PTRADD(x, y) ((void *) ((char *) (x) + (y)))

struct free_entry {
    struct free_entry *next;
    size_t size;
};

/* The kernel maintains two free lists, both protected by alloc_slock:
 *   - ksys_free_list:  regular allocations.
 *   - ksys_crit_list:  pre-reserved critical pool used as a last-resort
 *                     fall-back when the main list runs out (governed
 *                     by "alloc_critical").  We always start on the
 *                     regular list and bump up to the critical list
 *                     only when alloc() fails there.  Memory freed from
 *                     the critical region (detected by IS_CRIT() —
 *                     the region was carved as one contiguous blob at
 *                     init time, so any pointer in [crit_start,crit_end]
 *                     came from the critical list) goes back to it
 *                     directly, preventing a slow leak of critical
 *                     memory into the main list.
 *
 * One-list-per-CPU was rejected for the usual coalescing reason: a chunk freed
 * onto a different list than it was allocated from creates holes that can never
 * be merged back.
 *
 * The kernel allocator is single-heap (modulo the critical sub-list): U-mode
 * roottask runs its own libc malloc with its own arena, and the kernel never
 * allocates "on behalf of roottask".
 */

struct free_entry *ksys_free_list;
struct free_entry *ksys_crit_list;
uintptr_t crit_start = ~0UL; /* to make initial IS_CRIT() fail */
uintptr_t crit_end = 0;
static void *pregrow_start;
static size_t pregrow_size;

#define IS_CRIT(p) \
    (((uintptr_t) (p) >= crit_start) && ((uintptr_t) (p) <= crit_end))


/* _sreallocfunc — the heart of the kernel allocator.
 *
 * Heap choice is an explicit caller-supplied argument: HEAP_KER for the regular
 * kernel free list, HEAP_KER_CRIT to start directly on the critical sub-list.
 * alloc_slock — a leaf kspinlock under LOCK_CLASS_MEMPART — guards both lists;
 * it is dropped across the alloc() callback (vm_ops->mmap is far too long to
 * hold under a spinlock) and re-taken on return. Every exit path drops the
 * lock.
 *
 * Parameters:
 *   data     : existing allocation for realloc/free, or NULL for a fresh malloc
 *   old_size : size of the existing allocation (0 for a fresh malloc)
 *   new_size : requested new size (0 to free)
 *   alloc    : callback that obtains more memory from VM
 *   heap     : which free list to start on (HEAP_KER or HEAP_KER_CRIT)
 * Return value: pointer to the (re)allocated block, or NULL.
 */
void *_sreallocfunc(void *data,
                    size_t old_size,
                    size_t new_size,
                    size_t (*alloc)(size_t size, void **addr),
                    enum heap_choice heap)
{
    struct free_entry *p, *prev, *new, **pp, **fit, *add;
    int heap_tier;

    if (new_size) {
        new_size = new_size <= sizeof *p ? sizeof *p : ALIGN(new_size);
    }

    if ((uintptr_t) data & (_MALLOC_ALIGN - 1))
        crash();

    add = 0;

    /* Map heap_choice to the heap tier:
     *   heap_tier = 1: kernel main heap
     *   heap_tier = 2: kernel critical heap
     * The IS_CRIT(data) check below may bump tier 1 to 2 when freeing a pointer
     * that originated from the critical region; the alloc-failed fallback may
     * also bump 1 to 2 via "goto addmem".
     */
    heap_tier = (heap == HEAP_KER_CRIT) ? 2 : 1;
addmem:
    kspinlock_lock(&alloc_slock);
    if ((heap_tier == 1) && !IS_CRIT(data)) {
        fit = &ksys_free_list;
    } else {
        heap_tier = 2;
        fit = &ksys_crit_list;
    }
    pp = fit;

    p = prev = new = 0;
    if (data && old_size) {
        /* realloc and free */
        old_size = old_size < sizeof *p ? sizeof *p : ALIGN(old_size);

        if (new_size == old_size) {
            /* If no size change, we are done */
            kspinlock_unlock(&alloc_slock);
            return data;
        }
        if (new_size && new_size + sizeof *p <= old_size) {
            /* If shrinking, change to a free, but keep "new" to return */
            new = data;
            data = PTRADD(data, new_size);
            old_size -= new_size;
            new_size = 0;
        }

        for (pp = fit, fit = 0, prev = 0; (p = *pp); pp = &(prev = p)->next) {
            if (new_size && (p->size == new_size ||
                             (!fit && p->size >= new_size + sizeof *p))) {
                fit = pp;
            }

            if (data && ((uintptr_t) data >= (uintptr_t) p &&
                         (uintptr_t) data < ((uintptr_t) p + p->size))) {
                /* freed an already freed area */
                crash();

                /* NOTREACHED */
                kspinlock_unlock(&alloc_slock);
                return 0;
            }

            if (data && (uintptr_t) p > (uintptr_t) data) {
                break;
            }
        }
    }

    if (new_size) {
        if (data && PTRADD(data, old_size) == p) {
            if (new_size == old_size + p->size) {
                *pp = p->next;
                kspinlock_unlock(&alloc_slock);
                return data;
            } else if (new_size + sizeof *p <= old_size + p->size) {
                /* We must save "*p" as prev may overlap */
                struct free_entry save = *p;

                prev = PTRADD(data, new_size);
                prev->size = (old_size + save.size) - new_size;
                prev->next = save.next;
                *pp = prev;

                kspinlock_unlock(&alloc_slock);
                return data;
            }
        }

        for (fit = fit ? fit : pp; (new = *fit); fit = &new->next) {
            if (new->size == new_size) {
                *fit = new->next;
                if (prev == new) {
                    pp = fit;
                    prev = 0;
                }
                break;
            }
            if (new->size >= new_size + sizeof *new) {
                p = PTRADD(new, new_size);
                p->size = new->size - new_size;
                p->next = new->next;
                *fit = p;
                if (prev == new) {
                    pp = &p->next;
                    prev = p;
                }
                break;
            }
        }

        if (!new) {
            size_t size;

            /* when allocating make sure there is enough room of a new free
             * entry
             */
            if (alloc) {
                /* Drop the heap lock across the alloc() callback — vm_ops->mmap
                 * is unsafe under a spinlock and may itself take other heap
                 * locks recursively.
                 */
                kspinlock_unlock(&alloc_slock);

                if (heap_tier == 2) {
                    /* If we're working the critical list, we know there's no
                     * memory available from the vm_ops.
                     */
                    size = 0;
                } else {
                    size = alloc(new_size + sizeof *new, (void **) &new);
                }
                if (size) {
                    /* Re-take alloc_slock before mutating the kernel free list
                     * below.
                     */
                    kspinlock_lock(&alloc_slock);
                    if ((size -= new_size)) {
                        add = PTRADD(new, new_size);
                        add->size = size;
                    }
                } else {
                    /* NYI: Before going to the critical list or failing the
                     * allocation, we should tell all the soul_t lists to free
                     * any unneeded entries.
                     */
                    if ((heap_tier == 1) && alloc_critical) {
                        /* alloc failed and it's critical. Try again from the
                         * critical list (addmem will re-acquire alloc_slock).
                         */
                        heap_tier = 2;
                        goto addmem;
                    }
                    /* alloc failed, make sure we don't do a free */
                    new = 0;
                    data = 0;

                    /* Re-take the lock so the single exit point at function end
                     * has matching state.
                     */
                    kspinlock_lock(&alloc_slock);
                }
            }
        }
        if (data && new) {
            /* realloc to new area, so copy old data over */
            memcpy(new, data, min(new_size, old_size));
        }
    }
    if (data) {
#ifndef NDEBUG

        /* Erase data currently there - we pick something that will usually
         * generate an illegal address
         */
        memset(data, 0x6d, old_size);
#endif

        /* Free old entry, and coalesce if possible */
        if (prev && PTRADD(prev, prev->size) == data) {
            /* Previous free entry connects to this entry so join them */
            prev->size += old_size;
        } else {
            /* Make a new free entry for this data */
            prev = data;
            prev->next = *pp;
            prev->size = old_size;
            *pp = prev;
        }
        if (PTRADD(prev, prev->size) == (p = prev->next)) {
            /* Join this area with the free entry following this one */
            prev->next = p->next;
            prev->size += p->size;
        }
    }

    /* When allocating memory, extra was allocated, so add it to the free list
     */
    if (add) {
        if (alloc) {
            data = add;
            old_size = add->size;
            new_size = 0;
            add = new;
            alloc = 0;

            /* alloc_slock is currently held — drop it before goto so addmem's
             * re-acquire doesn't deadlock against ourselves.
             */
            kspinlock_unlock(&alloc_slock);
            goto addmem;
        }
        new = add;
    }

    if ((uintptr_t) new &(_MALLOC_ALIGN - 1))
        crash();

    kspinlock_unlock(&alloc_slock);

    return new;
}

/* Obtain more memory from VM for the kernel heap.
 *
 * Rounds the request up to a page, satisfying it from the pre-grown reserve set
 * up by heap_init() when possible, otherwise mapping fresh anonymous pages via
 * vm_ops->mmap. Parameters:
 *   size : number of bytes requested
 *   addr : receives the base address of the obtained region
 * Return value: number of bytes actually obtained, or 0 on failure.
 */
static size_t _heap_alloc(size_t size, void **addr)
{
    size = ROUNDUP(size, __PAGESIZE);
    if (size <= pregrow_size) {
        /* Can use the memory that was pre-allocated by heap_init() */
        *addr = pregrow_start;
        pregrow_size -= size;
        pregrow_start = (uint8_t *) pregrow_start + size;
        return size;
    }

    if (vm_ops->mmap(0, NULL, 0, size, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_ANON | MAP_PRIVATE, 0, 0, _MALLOC_ALIGN, 0, NOFD, addr,
                     &size) != EOK) {
        return 0;
    }
    return size;
}

/* Kernel-internal wrappers (_smalloc / _sfree / _scalloc / _srealloc). Single
 * heap (HEAP_KER, with HEAP_KER_CRIT as fall-back inside _sreallocfunc).
 */

/* Resize a kernel-heap allocation. Parameters:
 *   data     : existing block (NULL allocates)
 *   old_size : current size of the block
 *   new_size : requested new size
 * Return value: pointer to the resized block, or NULL.
 */
void *_srealloc(void *data, size_t old_size, size_t new_size)
{
    return _sreallocfunc(data, old_size, new_size, _heap_alloc, HEAP_KER);
}

/* Free a kernel-heap allocation.
 *
 * Re-aligns a misaligned pointer/size pair to the allocator's alignment before
 * returning the block to the free list. Parameters:
 *   data : block to free
 *   size : size of the block
 */
void _sfree(void *data, size_t size)
{
    if ((uintptr_t) data & (_MALLOC_ALIGN - 1)) {
        size -= _MALLOC_ALIGN - ((uintptr_t) data & (_MALLOC_ALIGN - 1));
        size &= ~(_MALLOC_ALIGN - 1);
        data = (char *) data + _MALLOC_ALIGN -
               ((uintptr_t) data & (_MALLOC_ALIGN - 1));
    };
    _sreallocfunc(data, size, 0, _heap_alloc, HEAP_KER);
}

/* Allocate uninitialized memory from the kernel heap. Parameters:
 *   size : number of bytes
 * Return value: pointer to the allocation, or NULL.
 */
void *_smalloc(size_t size)
{
    return _sreallocfunc(0, 0, size, _heap_alloc, HEAP_KER);
}

/* Allocate zeroed memory from the kernel heap. Parameters:
 *   size : number of bytes
 * Return value: pointer to the zeroed allocation, or NULL.
 */
void *_scalloc(size_t size)
{
    void *p;

    if ((p = _sreallocfunc(0, 0, size, _heap_alloc, HEAP_KER))) {
        memset(p, 0, size);
    }
    return p;
}

/* Allocate zeroed memory aligned to a page boundary. Over-allocates by one page
 * to guarantee alignment. The returned pointer cannot be individually freed.
 */
void *_scalloc_pgalign(size_t size)
{
    void *raw = _scalloc(size + __PAGESIZE);
    if (!raw)
        return NULL;
    return (void *) (((uintptr_t) raw + __PAGESIZE - 1) & ~(__PAGESIZE - 1));
}

/* Free pages handed back by _sfree_page, linked through their first word and
 * guarded by alloc_slock.
 */
static void *page_free_list;

/* Allocate one zeroed, page-aligned page that _sfree_page can take back. A
 * _scalloc_pgalign block cannot be freed exactly (its alignment slack is lost),
 * so pages that come and go, such as page tables, are recycled here instead:
 * the slack is paid once per page ever allocated, not once per allocation.
 * Return value: the page, or NULL.
 */
void *_scalloc_page(void)
{
    void *p;

    kspinlock_lock(&alloc_slock);
    if ((p = page_free_list))
        page_free_list = *(void **) p;
    kspinlock_unlock(&alloc_slock);
    if (!p)
        return _scalloc_pgalign(__PAGESIZE);
    memset(p, 0, __PAGESIZE);
    return p;
}

/* Return a page from _scalloc_page for reuse. Parameters:
 *   p : the page (NULL is a no-op)
 */
void _sfree_page(void *p)
{
    if (!p)
        return;
    kspinlock_lock(&alloc_slock);
    *(void **) p = page_free_list;
    page_free_list = p;
    kspinlock_unlock(&alloc_slock);
}

/* Free a block allocated by malloc/calloc/realloc.
 *
 * The block's size is recovered from the size_t header stored just below the
 * returned pointer. Parameters:
 *   data : block to free (NULL is a no-op)
 */
void free(void *data)
{
    size_t *p = data;

    if (p) {
        p = TOPTR(((char *) p - _MALLOC_ALIGN));
        _sreallocfunc(p, *p, 0, _heap_alloc, HEAP_KER);
    }
}

/* Allocate memory with a size header for later free()/realloc().
 *
 * Over-allocates by _MALLOC_ALIGN to stash the size below the returned pointer.
 * Parameters:
 *   size : number of bytes requested
 * Return value: pointer to the allocation, or NULL.
 */
void *malloc(size_t size)
{
    size_t *p;
    size += _MALLOC_ALIGN;
    if ((p = _sreallocfunc(0, 0, size, _heap_alloc, HEAP_KER))) {
        *p = size;
        p = TOPTR(((char *) p + _MALLOC_ALIGN));
    }
    return p;
}

/* Allocate zeroed memory with a size header for later free()/realloc().
 * Parameters:
 *   size : size of each element
 *   num  : number of elements
 * Return value: pointer to the zeroed allocation, or NULL.
 */
void *calloc(size_t size, size_t num)
{
    size_t *p;

    size = size * num + _MALLOC_ALIGN;
    if ((p = _sreallocfunc(0, 0, size, _heap_alloc, HEAP_KER))) {
        memset(p, 0x00, size);
        *p = size;
        p = TOPTR(((char *) p + _MALLOC_ALIGN));
    }
    return p;
}

/* Resize a block allocated by malloc/calloc/realloc.
 *
 * Recovers the old size from the header below "data", then re-stamps the header
 * on the resized block. Parameters:
 *   data : existing block (NULL allocates)
 *   size : requested new size (0 frees and returns NULL)
 * Return value: pointer to the resized block, or NULL.
 */
void *realloc(void *data, size_t size)
{
    size_t *p, old_size;

    old_size = 0;
    if ((p = data)) {
        p = TOPTR(((char *) p - _MALLOC_ALIGN));
        old_size = *p;
    }
    if (size) {
        size += _MALLOC_ALIGN;
    }
    if ((p = _sreallocfunc(p, old_size, size, _heap_alloc, HEAP_KER))) {
        if (size) {
            *p = size;
            p = TOPTR(((char *) p + _MALLOC_ALIGN));
        } else {
            p = 0;
        }
    }
    return p;
}

#define CRIT_SIZE (4 * __PAGESIZE)

/* Initialize the kernel heap.
 *
 * Sets up alloc_slock, optionally pre-grows the heap, and carves out the
 * contiguous critical-allocation reserve. Runs once at boot on the boot CPU
 * only, so it is uncontended. Parameters:
 *   pregrow : number of bytes to pre-allocate for the main heap (0 to skip)
 */
void heap_init(size_t pregrow)
{
    /* alloc_slock must be ready before the very first heap operation:
     * _heap_alloc / _smalloc below both go through _sreallocfunc which
     * unconditionally takes alloc_slock on the kernel-heap path. We are still
     * single-CPU here (boot CPU only), so the init itself is uncontended.
     */
    kspinlock_init(&alloc_slock, LOCK_CLASS_MEMPART, "alloc_slock");

    if (pregrow != 0) {
        /* Should I loop with smaller sizes if the alloc fails? */
        pregrow_size = _heap_alloc(pregrow, &pregrow_start);
    }

    /* Preallocate some pages of memory for critical allocations. We only dip
     * into these when the alloc_critical counter is non-zero
     */

    ksys_crit_list = _smalloc(CRIT_SIZE);
    ksys_crit_list->next = NULL;
    ksys_crit_list->size = CRIT_SIZE;
    crit_start = (uintptr_t) ksys_crit_list;
    crit_end = crit_start + (CRIT_SIZE - 1);
}


/* Free a block back to the critical reserve if it came from there. Parameters:
 *   p    : block to free
 *   size : size of the block
 * Return value: 1 if the block was from the critical area and freed, 0
 * otherwise.
 */
int crit_sfree(void *p, size_t size)
{
    /* If the pointer is from the critical area, we want to put it back in the
     * critical free list as soon as possible so that it's available for future
     * allocations.
     */
    if (IS_CRIT(p)) {
        _sfree(p, size);
        return 1;
    }
    return 0;
}
