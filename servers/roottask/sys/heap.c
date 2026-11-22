/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Roottask's malloc backing store.
 *
 * Roottask is the mmap server, so libc's default allocator backend (which maps
 * anonymous memory through roottask) cannot work here. These strong definitions
 * replace libc's weak __crt_morecore/__crt_lesscore with a fixed arena carved
 * at link time.
 *
 * Blocks come from a bump pointer and go back to a list of free extents kept in
 * address order, merged with their neighbours and, at the top, with the bump
 * region. Directly mapped blocks (__crt_large_min and up) are the ones that
 * come back, so a large request that is freed leaves its bytes for any later
 * request of any size, not for one size class only.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdalign.h>
#include <pthread.h>
#include <string.h>
#ifndef CONFIG_ROOTTASK_HEAP_SIZE
#include <generated/autoconf.h>
#endif
#include "heap-stats.h"
#include "root-mutex.h"

#define ROOTTASK_HEAP_PAGE 4096

_Static_assert(CONFIG_ROOTTASK_HEAP_SIZE % ROOTTASK_HEAP_PAGE == 0,
               "CONFIG_ROOTTASK_HEAP_SIZE must be a multiple of 4096");

static alignas(
    ROOTTASK_HEAP_PAGE) char roottask_heap[CONFIG_ROOTTASK_HEAP_SIZE];

/* Bytes of the arena below the bump pointer. Guarded by ext_lock. */
static size_t roottask_heap_used;

/* Free extents come only from mapped blocks, which are whole pages, and two
 * extents are apart by at least one live page, so the arena holds fewer than
 * this many and the table cannot overflow. Giving a block up when it does
 * (ext_dropped) is only a guard.
 */
#define HEAP_LARGE_MIN (32 * 1024)
#define FREE_EXTENTS (CONFIG_ROOTTASK_HEAP_SIZE / (2 * ROOTTASK_HEAP_PAGE) + 1)
static struct {
    char *p;
    size_t n;
} free_ext[FREE_EXTENTS];
static unsigned nfree;
static size_t ext_dropped;

/* A priority-inheritance mutex, statically initialized so that taking it never
 * allocates (see alloc_mutex in lib/libc/alloc/malloc-core.c): a spinner on a
 * lock held by a preempted lower-priority thread of the same hart would never
 * let the holder run.
 */
#ifdef MALLOC_HOST_TEST
static pthread_mutex_t ext_mutex = PTHREAD_MUTEX_INITIALIZER;
#else
static pthread_mutex_t ext_mutex = {RTMUX_SYNC_NONRECURSIVE,
                                    RTMUX_SYNC_INITIALIZER};
#endif

/* Smallest request malloc maps directly. Roottask sets it well below libc's 128
 * KiB so that the 32 and 64 KiB buffers of a large spawn are returned here and
 * merged, instead of becoming size-class blocks that keep their bytes for one
 * class for ever (K43).
 */
size_t __crt_large_min = HEAP_LARGE_MIN;

void *__crt_morecore(size_t n);
void __crt_lesscore(void *p, size_t n);

static void ext_acquire(void)
{
    root_mutex_lock(&ext_mutex);
}

static void ext_release(void)
{
    (void) pthread_mutex_unlock(&ext_mutex);
}

void roottask_heap_stats(struct roottask_heap_stats *st)
{
    ext_acquire();
    st->arena = sizeof roottask_heap;
    st->used = roottask_heap_used;
    st->extents = nfree;
    st->extent_bytes = 0;
    st->largest = sizeof roottask_heap - roottask_heap_used;
    st->dropped = ext_dropped;
    for (unsigned i = 0; i < nfree; i++) {
        st->extent_bytes += free_ext[i].n;
        if (free_ext[i].n > st->largest)
            st->largest = free_ext[i].n;
    }
    ext_release();
}

static void ext_remove(unsigned i)
{
    memmove(&free_ext[i], &free_ext[i + 1],
            (nfree - i - 1) * sizeof free_ext[0]);
    nfree--;
}

/* Take n bytes: the smallest extent that holds them, else the bump region. */
void *__crt_morecore(size_t n)
{
    void *p = NULL;
    unsigned best;

    ext_acquire();
    best = nfree;
    for (unsigned i = 0; i < nfree; i++)
        if (free_ext[i].n >= n &&
            (best == nfree || free_ext[i].n < free_ext[best].n)) {
            best = i;
            if (free_ext[i].n == n)
                break;
        }
    if (best < nfree) {
        p = free_ext[best].p;
        free_ext[best].p += n;
        free_ext[best].n -= n;
        if (free_ext[best].n == 0)
            ext_remove(best);
    } else if (n <= sizeof roottask_heap - roottask_heap_used) {
        p = roottask_heap + roottask_heap_used;
        roottask_heap_used += n;
    }
    ext_release();
    return p;
}

/* Take a block back. It joins the extents next to it; one that ends at the bump
 * pointer lowers the pointer instead.
 */
void __crt_lesscore(void *ptr, size_t n)
{
    char *p = ptr;
    unsigned i = 0;
    bool prev_adj, next_adj;

    ext_acquire();
    if (p + n == roottask_heap + roottask_heap_used) {
        /* The top of the arena goes back to the bump region, and with it any
         * extent that then ends at the new top.
         */
        roottask_heap_used = (size_t) (p - roottask_heap);
        while (nfree && free_ext[nfree - 1].p + free_ext[nfree - 1].n ==
                            roottask_heap + roottask_heap_used) {
            roottask_heap_used =
                (size_t) (free_ext[nfree - 1].p - roottask_heap);
            nfree--;
        }
        ext_release();
        return;
    }
    while (i < nfree && free_ext[i].p < p)
        i++;
    prev_adj = i > 0 && free_ext[i - 1].p + free_ext[i - 1].n == p;
    next_adj = i < nfree && p + n == free_ext[i].p;
    if (nfree == FREE_EXTENTS && !prev_adj && !next_adj) {
        ext_dropped += n;
        ext_release();
        return;
    }
    if (prev_adj) {
        free_ext[--i].n += n;
    } else if (next_adj) {
        free_ext[i].p = p;
        free_ext[i].n += n;
    } else {
        /* Insert at i, which keeps the table in address order. */
        memmove(&free_ext[i + 1], &free_ext[i],
                (nfree - i) * sizeof free_ext[0]);
        nfree++;
        free_ext[i].p = p;
        free_ext[i].n = n;
    }
    if (i + 1 < nfree && free_ext[i].p + free_ext[i].n == free_ext[i + 1].p) {
        free_ext[i].n += free_ext[i + 1].n;
        ext_remove(i + 1);
    }
    ext_release();
}
