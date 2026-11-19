/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The log line for a failed roottask allocation (K43). */

#include "externs.h"
#include <stdatomic.h>
#include <sys/mallocstats.h>

/* Only the first few failures and every 64th after are logged, so a client that
 * keeps asking cannot flood the ring buffer.
 */
void roottask_heap_note_nomem(const char *what, size_t want)
{
    static atomic_uint nfail;
    struct roottask_heap_stats hs;
    struct __rtmux_mallocstats ms;
    unsigned n = atomic_fetch_add_explicit(&nfail, 1, memory_order_relaxed);

    if (n >= 8 && (n & 63))
        return;
    roottask_heap_stats(&hs);
    (void) __rtmux_mallocstats(&ms);
    root_warn(
        "nomem: %s failed for %zu bytes (failure %u): arena %zu used %zu "
        "free extents %u/%zu largest %zu dropped %zu live %llu peak %llu\n",
        what, want, n + 1, hs.arena, hs.used, hs.extents, hs.extent_bytes,
        hs.largest, hs.dropped, ms.bytes_in_use, ms.bytes_peak);
}

/* probe: the old heap keeps no such statistics */
void roottask_heap_stats(struct roottask_heap_stats *st)
{
    *st = (struct roottask_heap_stats){0};
}
