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
static pthread_mutex_t alloc_mutex = {RTMUX_SYNC_NONRECURSIVE,
                                      RTMUX_SYNC_INITIALIZER};

static inline void alloc_lock(void)
{
    (void) pthread_mutex_lock(&alloc_mutex);
}

static inline void alloc_unlock(void)
{
    (void) pthread_mutex_unlock(&alloc_mutex);
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
    return base + LARGE_HDR;
}

void *__crt_malloc(size_t nbytes)
{
    union overhead *op;
    int bucket;
    size_t amt;
    void *ret;

    if (nbytes >= LARGE_MIN)
        return large_alloc(nbytes);

    amt = FIRST_BUCKET_SIZE;
    bucket = 0;
    while (nbytes > amt - sizeof(*op)) {
        amt <<= 1;
        bucket++;
        if (amt == 0 || bucket >= NBUCKETS)
            return NULL;
    }

    alloc_lock();
    while (!(op = nextf[bucket])) {
        if (!refill(bucket)) {
            alloc_unlock();
            return NULL;
        }
    }
    nextf[bucket] = op->ov_next;
    op->ov_magic = MAGIC;
    op->ov_index = (uint16_t) bucket;
    ret = (char *) (op + 1);
    alloc_unlock();
    return ret;
}

void *__crt_calloc(size_t num, size_t size)
{
    void *ret;

    if (size != 0 && (num * size) / size != num)
        return NULL; /* overflow */

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
    if (align & (align - 1))
        return NULL;
    offset &= align - 1;
    if (size > SIZE_MAX - align || size + align > SIZE_MAX - offset ||
        size + align + offset > SIZE_MAX - sizeof(union overhead))
        return NULL;
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
        if (!chunk)
            return 0;
        if (nextf[bucket] || (size_t) (pagepool_end - pagepool_start) >= amt) {
            /* Another thread refilled while the lock was dropped. */
            donate(chunk, want);
            return 1;
        }
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

        __crt_lesscore(base, *(size_t *) (void *) base);
        return;
    }
    if (op->ov_magic != MAGIC)
        return; /* sanity */
    size = op->ov_index;
    alloc_lock();
    op->ov_next = nextf[size];
    nextf[size] = op;
    alloc_unlock();
}

void *__crt_realloc(void *cp, size_t nbytes)
{
    size_t onb;
    int i;
    union overhead *op, op1;
    char *res;

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
            return NULL;
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
         * shrinking below LARGE_MIN moves to the bucketed heap.
         */
        if (nbytes >= LARGE_MIN && nbytes <= cap)
            return cp;
        if (!(res = __crt_malloc(nbytes)))
            return NULL;
        memcpy(res, cp, nbytes < cap ? nbytes : cap);
        __crt_free(cp);
        return res;
    }
    if (op->ov_magic != MAGIC)
        return NULL;
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
}
