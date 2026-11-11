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
 * at link time. Freed large blocks are reused, never returned to the system.
 */

#include <stddef.h>
#include <stdalign.h>
#include <stdatomic.h>
#include <generated/autoconf.h>

#define ROOTTASK_HEAP_PAGE 4096

_Static_assert(CONFIG_ROOTTASK_HEAP_SIZE % ROOTTASK_HEAP_PAGE == 0,
               "CONFIG_ROOTTASK_HEAP_SIZE must be a multiple of 4096");

static alignas(
    ROOTTASK_HEAP_PAGE) char roottask_heap[CONFIG_ROOTTASK_HEAP_SIZE];
static size_t roottask_heap_used;

/* Directly mapped blocks (128 KiB and up) come back through __crt_lesscore.
 * They are kept here and reused first-fit, so a fixed arena does not leak on
 * every large free. A small table is enough: roottask makes few large
 * allocations, and an extent that does not fit the table is simply dropped.
 * Called with malloc's lock not held, so the table has its own lock.
 */
#define FREE_EXTENTS 8
static struct {
    char *p;
    size_t n;
} free_ext[FREE_EXTENTS];
static atomic_uint ext_lock;

void *__crt_morecore(size_t n);
void __crt_lesscore(void *p, size_t n);

static void ext_acquire(void)
{
    while (atomic_exchange_explicit(&ext_lock, 1, memory_order_acquire))
        ;
}

static void ext_release(void)
{
    atomic_store_explicit(&ext_lock, 0, memory_order_release);
}

void *__crt_morecore(size_t n)
{
    void *p = NULL;

    ext_acquire();
    for (int i = 0; i < FREE_EXTENTS && !p; i++) {
        if (free_ext[i].p && free_ext[i].n >= n) {
            p = free_ext[i].p;
            free_ext[i].p += n;
            free_ext[i].n -= n;
            if (free_ext[i].n == 0)
                free_ext[i].p = NULL;
        }
    }
    if (!p && n <= sizeof roottask_heap - roottask_heap_used) {
        p = roottask_heap + roottask_heap_used;
        roottask_heap_used += n;
    }
    ext_release();
    return p;
}

void __crt_lesscore(void *p, size_t n)
{
    ext_acquire();
    for (int i = 0; i < FREE_EXTENTS; i++) {
        if (!free_ext[i].p) {
            free_ext[i].p = p;
            free_ext[i].n = n;
            break;
        }
    }
    ext_release();
}
