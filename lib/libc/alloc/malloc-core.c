/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1983 Regents of the University of California.
 * All rights reserved.
 *
 * This is a very fast storage allocator. It allocates blocks of a small number
 * of different sizes, and keeps free lists of each size. Blocks that don't
 * exactly fit are passed up to the next larger size.
 *
 * Adapted for RTMux. Backing store comes from two weak hooks, __crt_morecore(n)
 * and __crt_lesscore(p, n). The libc default maps and unmaps anonymous memory,
 * so a process heap grows on demand. Roottask is the mmap server and cannot use
 * that, so servers/roottask/sys/heap.c overrides both with a static arena. A
 * request of LARGE_MIN bytes or more gets its own mapping and is returned to
 * the system on free. malloc is not async-signal-safe.
 *
 * Statistics: five counters (see struct __rtmux_mallocstats in
 * <sys/mallocstats.h>). Small blocks are counted inside the critical sections
 * the allocator already has, with a plain load and store each (the lock makes
 * the writers one at a time), so counting adds no atomic read-modify-write. A
 * mapped block never takes the lock: it has counters of its own, updated
 * atomically, and the two sets are added when read. So are the failure count
 * and the peak, which is raised by compare and exchange only when exceeded. A
 * block's size is its usable payload, the bucket size less the header or the
 * mapping less LARGE_HDR, which is all free can know. __crt_mallocstats reads
 * the counters one at a time without the lock, so a snapshot taken while other
 * threads allocate need not be consistent; with none it is exact.
 */

#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdalign.h>
#include <stdint.h>
#include <string.h>
#include <align.h>
#include <sys/mman.h>
#include "malloc-core.h"

#define RTMUX_PAGE_SIZE 4096
#define CHUNK_SIZE (256 * 1024) /* backing requested per refill */
#define LARGE_MIN (128 * 1024)  /* requests this big are mapped directly */
#define LARGE_HDR 16            /* size_t mapping length + overhead word */

static char *pagepool_start; /* unused tail of the current chunk */
static char *pagepool_end;

static int pagesz = RTMUX_PAGE_SIZE;

/* Backing store hooks. Weak so roottask can supply its own arena.
 * __crt_morecore returns n bytes (a page multiple) or NULL.
 */
__attribute__((weak)) void *__crt_morecore(size_t n)
{
    void *p =
        mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);

    return p == MAP_FAILED ? NULL : p;
}

__attribute__((weak)) void __crt_lesscore(void *p, size_t n)
{
    (void) munmap(p, n);
}

/* Smallest request that is mapped directly rather than cut from a size class.
 * Weak so roottask, whose arena is fixed and whose backing store takes mapped
 * blocks back and merges them, can lower it: a size-class block is never
 * merged, so a one-off 64 KiB request would otherwise keep a 128 KiB class
 * block that nothing else can use (K43).
 */
__attribute__((weak, visibility("hidden"))) size_t __crt_large_min = LARGE_MIN;

/* Overhead per allocated block */

union overhead {
    union overhead *ov_next; /* when free */
    struct {
        uint16_t ovu_index; /* bucket # */
        uint8_t ovu_magic;  /* magic number */
    } ovu;
#define ov_magic ovu.ovu_magic
#define ov_index ovu.ovu_index
};

#define MAGIC 0xef
#define AMAGIC 0xdf
#define LMAGIC 0xcf /* directly mapped block */

/* Free lists */

#define LOW_BITS 3
#define FIRST_BUCKET_SIZE (1U << LOW_BITS)
#define NBUCKETS 30

static union overhead *nextf[NBUCKETS];

/* Allocator lock. The free lists are global, so concurrent malloc/free from
 * several threads (roottask's pool threads, any multithreaded program) must be
 * serialized.
 *
 * A priority-inheritance mutex, not a spinlock: a spinner never yields, so a
 * higher-priority thread spinning on a lock held by a preempted lower-priority
 * thread on the same hart starves the holder forever (roottask hung exactly
 * this way). The mutex blocks the waiter and boosts the holder. It is
 * statically initialized, so no allocation is needed to take it, and PRIONONE
 * is left clear, which selects priority inheritance. It must not use
 * CONFIG_ULOCK_DEBUG tracking, whose acquire hook allocates and would recurse.
 */
#ifdef MALLOC_HOST_TEST
static pthread_mutex_t alloc_mutex = PTHREAD_MUTEX_INITIALIZER;
#else
static pthread_mutex_t alloc_mutex = {RTMUX_SYNC_NONRECURSIVE,
                                      RTMUX_SYNC_INITIALIZER};
#endif

static inline void alloc_lock(void)
{
    (void) pthread_mutex_lock(&alloc_mutex);
}

static inline void alloc_unlock(void)
{
    (void) pthread_mutex_unlock(&alloc_mutex);
}

/* Statistics. MALLOC_NO_STATS builds the allocator without them, for the host
 * benchmark that measures what they cost. The small-block counters (st_*) have
 * one writer at a time, the holder of alloc_mutex. A mapped block is outside
 * that lock, so it has counters of its own (stm_*) updated atomically and never
 * waits for the lock; the sums are what is reported.
 */
static _Atomic unsigned long long st_allocs, st_frees, st_in_use;

/* Each group is on a cache line of its own: mapped blocks write stm without the
 * lock and must not take the line from a holder updating st_*, and stx.peak and
 * stx.failed are written rarely and must not share one with either.
 */
static struct {
    _Alignas(64) _Atomic unsigned long long allocs, frees, in_use;
} stm;
static struct {
    _Alignas(64) _Atomic unsigned long long peak, failed;
} stx;

#ifndef MALLOC_NO_STATS
/* A plain load and store of an atomic: only for writers holding alloc_mutex. */
static inline void st_add(_Atomic unsigned long long *c, unsigned long long n)
{
    atomic_store_explicit(c, atomic_load_explicit(c, memory_order_relaxed) + n,
                          memory_order_relaxed);
}

/* Raise the peak to `now` if it is higher. The compare is almost always false
 * once the program has warmed up, so the hot path costs one load.
 */
static inline void st_peak_raise(unsigned long long now)
{
    unsigned long long peak =
        atomic_load_explicit(&stx.peak, memory_order_relaxed);

    while (now > peak && !atomic_compare_exchange_weak_explicit(
                             &stx.peak, &peak, now, memory_order_relaxed,
                             memory_order_relaxed))
        ;
}

/* The caller holds alloc_mutex. */
static inline void stat_alloc(size_t usable)
{
    unsigned long long use =
        atomic_load_explicit(&st_in_use, memory_order_relaxed) + usable;

    st_add(&st_allocs, 1);
    atomic_store_explicit(&st_in_use, use, memory_order_relaxed);
    st_peak_raise(use +
                  atomic_load_explicit(&stm.in_use, memory_order_relaxed));
}

/* The caller holds alloc_mutex. */
static inline void stat_free(size_t usable)
{
    st_add(&st_frees, 1);
    st_add(&st_in_use, -(unsigned long long) usable);
}

static inline void stat_fail(void)
{
    atomic_fetch_add_explicit(&stx.failed, 1, memory_order_relaxed);
}

/* A mapped block: counted without the lock. A free is counted before the unmap,
 * an allocation after the map, so the counters never show a block as live while
 * another takes its place.
 */
static void stat_alloc_mapped(size_t usable)
{
    unsigned long long use =
        atomic_fetch_add_explicit(&stm.in_use, usable, memory_order_relaxed) +
        usable;

    atomic_fetch_add_explicit(&stm.allocs, 1, memory_order_relaxed);
    st_peak_raise(use + atomic_load_explicit(&st_in_use, memory_order_relaxed));
}

static void stat_free_mapped(size_t usable)
{
    atomic_fetch_add_explicit(&stm.frees, 1, memory_order_relaxed);
    atomic_fetch_sub_explicit(&stm.in_use, usable, memory_order_relaxed);
}
#else
#define stat_alloc(usable) ((void) (usable))
#define stat_free(usable) ((void) (usable))
#define stat_fail() ((void) 0)
#define stat_alloc_mapped(usable) ((void) (usable))
#define stat_free_mapped(usable) ((void) (usable))
#endif

void __crt_mallocstats(struct __rtmux_mallocstats *st)
{
    st->allocs = atomic_load_explicit(&st_allocs, memory_order_relaxed) +
                 atomic_load_explicit(&stm.allocs, memory_order_relaxed);
    st->frees = atomic_load_explicit(&st_frees, memory_order_relaxed) +
                atomic_load_explicit(&stm.frees, memory_order_relaxed);
    st->bytes_in_use = atomic_load_explicit(&st_in_use, memory_order_relaxed) +
                       atomic_load_explicit(&stm.in_use, memory_order_relaxed);
    st->bytes_peak = atomic_load_explicit(&stx.peak, memory_order_relaxed);
    st->failed = atomic_load_explicit(&stx.failed, memory_order_relaxed);
}

static void *cp2op(void *cp)
{
    return (char *) cp - sizeof(union overhead);
}

/* Core allocator (from FreeBSD malloc-core.c) */

static int refill(int bucket);

/* Directly mapped block: [size_t maplen][overhead word][payload...]. */
static void *large_alloc(size_t nbytes)
{
    size_t total;
    char *base;
    union overhead *op;

    if (nbytes > SIZE_MAX - LARGE_HDR - (RTMUX_PAGE_SIZE - 1))
        return NULL;
    total = (nbytes + LARGE_HDR + (RTMUX_PAGE_SIZE - 1)) &
            ~(size_t) (RTMUX_PAGE_SIZE - 1);
    if (!(base = __crt_morecore(total)))
        return NULL;
    *(size_t *) (void *) base = total;
    op = (union overhead *) (void *) (base + LARGE_HDR - sizeof(*op));
    op->ov_magic = LMAGIC;
    op->ov_index = 0;
    stat_alloc_mapped(total - LARGE_HDR);
    return base + LARGE_HDR;
}

void *__crt_malloc(size_t nbytes)
{
    union overhead *op;
    int bucket;
    size_t amt;
    void *ret;

    if (nbytes >= __crt_large_min) {
        if (!(ret = large_alloc(nbytes)))
            stat_fail();
        return ret;
    }

    amt = FIRST_BUCKET_SIZE;
    bucket = 0;
    while (nbytes > amt - sizeof(*op)) {
        amt <<= 1;
        bucket++;
        if (amt == 0 || bucket >= NBUCKETS) {
            stat_fail();
            return NULL;
        }
    }

    alloc_lock();
    while (!(op = nextf[bucket])) {
        if (!refill(bucket)) {
            alloc_unlock();
            stat_fail();
            return NULL;
        }
    }
    nextf[bucket] = op->ov_next;
    op->ov_magic = MAGIC;
    op->ov_index = (uint16_t) bucket;
    ret = (char *) (op + 1);
    stat_alloc(amt - sizeof(*op));
    alloc_unlock();
    return ret;
}

void *__crt_calloc(size_t num, size_t size)
{
    void *ret;

    if (size != 0 && (num * size) / size != num) {
        stat_fail();
        return NULL; /* overflow */
    }

    if ((ret = __crt_malloc(num * size)))
        memset(ret, 0, num * size);
    return ret;
}

void *__crt_aligned_alloc_offset(size_t align, size_t size, size_t offset)
{
    void *mem;
    union overhead ov1;
    uintptr_t x;

    if (align < FIRST_BUCKET_SIZE)
        align = FIRST_BUCKET_SIZE;
    /* lib_align_up() below assumes a power-of-two alignment. */
    if (align & (align - 1)) {
        stat_fail();
        return NULL;
    }
    offset &= align - 1;
    if (size > SIZE_MAX - align || size + align > SIZE_MAX - offset ||
        size + align + offset > SIZE_MAX - sizeof(union overhead)) {
        stat_fail();
        return NULL;
    }
    mem = __crt_malloc(size + align + offset + sizeof(union overhead));
    if (!mem)
        return NULL;
    x = lib_align_up((uintptr_t) mem + sizeof(union overhead), align);
    x += offset;

    /* ov_index stores the byte distance back to the real overhead and is only a
     * uint16_t; a large alignment would truncate it and make free() compute the
     * wrong header. Reject what we can't encode.
     */
    size_t idx = x - (uintptr_t) mem + sizeof(union overhead);
    if (idx > UINT16_MAX) {
        __crt_free(mem);
        stat_fail();
        return NULL;
    }
    ov1.ov_magic = AMAGIC;
    ov1.ov_index = (uint16_t) idx;
    memcpy(cp2op((void *) x), &ov1, sizeof(ov1));
    return (void *) x;
}

/* Push [p, p + len) onto the free lists as the largest power-of-two blocks that
 * fit. Caller holds the lock.
 */
static void donate(char *p, size_t len)
{
    while (len >= FIRST_BUCKET_SIZE) {
        size_t sz = FIRST_BUCKET_SIZE;
        int b = 0;
        union overhead *op;

        while (b + 1 < NBUCKETS && (sz << 1) <= len) {
            sz <<= 1;
            b++;
        }
        op = (union overhead *) (void *) p;
        op->ov_next = nextf[b];
        nextf[b] = op;
        p += sz;
        len -= sz;
    }
}

/* Backing memory ran out: cut a cached free block of a larger class down to
 * this one. The pieces are never merged back (the allocator has no coalescing),
 * so this only hands memory a class no longer needs to a class that has none.
 * It leaves one block of each class from bucket up to the donor's, and puts the
 * first into nextf[bucket]. Caller holds the lock.
 *
 * Returns 0 if no larger class has a free block.
 */
static int steal(int bucket)
{
    for (int b = bucket + 1; b < NBUCKETS; b++) {
        union overhead *op = nextf[b];
        char *p;

        if (!op)
            continue;
        nextf[b] = op->ov_next;
        p = (char *) op;
        for (int c = b - 1; c >= bucket; c--) {
            union overhead *half =
                (union overhead *) (void *) (p +
                                             ((size_t) FIRST_BUCKET_SIZE << c));

            half->ov_next = nextf[c];
            nextf[c] = half;
        }
        op->ov_next = nextf[bucket];
        nextf[bucket] = op;
        return 1;
    }
    return 0;
}

/* Make nextf[bucket] non-empty. Called and returns with the lock held, but
 * drops it while asking for backing memory.
 *
 * Returns 0 if memory ran out.
 */
static int refill(int bucket)
{
    union overhead *op;
    size_t sz = (size_t) FIRST_BUCKET_SIZE << bucket;
    size_t amt = sz < (size_t) pagesz ? (size_t) pagesz : sz;
    size_t nblks = amt / sz;

    if ((size_t) (pagepool_end - pagepool_start) < amt) {
        size_t want = amt > CHUNK_SIZE ? amt : CHUNK_SIZE;
        char *chunk;

        alloc_unlock();
        chunk = __crt_morecore(want);
        if (!chunk && want > amt) {
            want = amt;
            chunk = __crt_morecore(want);
        }
        alloc_lock();
        if (nextf[bucket] || (size_t) (pagepool_end - pagepool_start) >= amt) {
            /* Another thread refilled while the lock was dropped. */
            if (chunk)
                donate(chunk, want);
            return 1;
        }
        if (!chunk)
            return steal(bucket);
        donate(pagepool_start, (size_t) (pagepool_end - pagepool_start));
        pagepool_start = chunk;
        pagepool_end = chunk + want;
    }

    op = (union overhead *) (void *) pagepool_start;
    pagepool_start += amt;

    nextf[bucket] = op;
    while (--nblks > 0) {
        op->ov_next = (union overhead *) (void *) ((char *) op + sz);
        op = (union overhead *) (void *) ((char *) op + sz);
    }
    op->ov_next = NULL;
    return 1;
}

void __crt_free(void *cp)
{
    union overhead *op, op1;
    void *opx;
    int size;

    if (!cp)
        return;
    opx = cp2op(cp);
    memcpy(&op1, opx, sizeof(op1));
    op = (op1.ov_magic == AMAGIC) ? (void *) ((char *) cp - op1.ov_index) : opx;
    if (op->ov_magic == LMAGIC) {
        char *base = (char *) op - (LARGE_HDR - sizeof(*op));
        size_t total = *(size_t *) (void *) base;

        stat_free_mapped(total - LARGE_HDR);
        __crt_lesscore(base, total);
        return;
    }
    if (op->ov_magic != MAGIC)
        return; /* sanity */
    size = op->ov_index;
    alloc_lock();
    op->ov_next = nextf[size];
    nextf[size] = op;
    stat_free(((size_t) FIRST_BUCKET_SIZE << size) - sizeof(*op));
    alloc_unlock();
}

/* realloc. A pointer that is not a live block counts as a failed request, and
 * *bad (if not NULL) is set to tell it from an allocation failure.
 */
void *__crt_realloc(void *cp, size_t nbytes, int *bad)
{
    size_t onb;
    int i;
    union overhead *op, op1;
    char *res;

    if (bad)
        *bad = 0;
    if (!cp)
        return __crt_malloc(nbytes);
    memcpy(&op1, cp2op(cp), sizeof(op1));
    if (op1.ov_magic == AMAGIC) {
        /* Aligned block: the real header sits at the block start, ov_index
         * bytes before cp — not immediately before it. The in-place shrink path
         * below assumes cp == block start, so handle aligned blocks the generic
         * way (malloc + copy + free). usable is the span from cp to the end of
         * the bucket, which is what realloc may safely copy.
         */
        op = (union overhead *) ((char *) cp - op1.ov_index);
        size_t usable;

        if (op->ov_magic == LMAGIC) {
            char *base = (char *) op - (LARGE_HDR - sizeof(*op));

            usable =
                (size_t) ((base + *(size_t *) (void *) base) - (char *) cp);
        } else if (op->ov_magic == MAGIC) {
            size_t blksz = (size_t) FIRST_BUCKET_SIZE << op->ov_index;

            usable = (size_t) (((char *) op + blksz) - (char *) cp);
        } else {
            goto invalid;
        }
        if (nbytes <= usable)
            return cp;
        if (!(res = __crt_malloc(nbytes)))
            return NULL;
        memcpy(res, cp, usable);
        __crt_free(cp);
        return res;
    }
    op = cp2op(cp);
    if (op->ov_magic == LMAGIC) {
        char *base = (char *) op - (LARGE_HDR - sizeof(*op));
        size_t cap = *(size_t *) (void *) base - LARGE_HDR;

        /* Keep the mapping while the new size still belongs in a mapped block;
         * shrinking below __crt_large_min moves to the bucketed heap.
         */
        if (nbytes >= __crt_large_min && nbytes <= cap)
            return cp;
        if (!(res = __crt_malloc(nbytes)))
            return NULL;
        memcpy(res, cp, nbytes < cap ? nbytes : cap);
        __crt_free(cp);
        return res;
    }
    if (op->ov_magic != MAGIC)
        goto invalid;
    i = op->ov_index;

    /* Usable payload is the bucket size minus the header for every bucket:
     * __crt_malloc() places a request in bucket b when nbytes <=
     * (FIRST_BUCKET_SIZE << b) - sizeof(*op), and morecore() hands out exactly
     * that many bytes per block on both the sub-page and the one-block-per-page
     * paths. The classic BSD "+ pagesz" for large blocks matched a morecore
     * that over-allocated them by a page; this port does not, so keeping it
     * overstated the block by pagesz and let realloc grow in place past the
     * allocation — a heap overflow.
     */
    onb = ((size_t) 1 << (i + 3)) - sizeof(*op);
    /* avoid the copy if same size block */
    if (i != 0) {
        size_t lower = ((size_t) 1 << (i + 2)) - sizeof(*op);
        if (nbytes <= onb && nbytes > lower)
            return cp;
    } else {
        if (nbytes <= onb)
            return cp;
    }
    if (!(res = __crt_malloc(nbytes)))
        return NULL;
    memcpy(res, cp, (nbytes < onb) ? nbytes : onb);
    __crt_free(cp);
    return res;

invalid:
    stat_fail();
    if (bad)
        *bad = 1;
    return NULL;
}
