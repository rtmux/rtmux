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

/* Rate limited (root_warn_due), so a client that keeps asking cannot flood the
 * ring buffer.
 */
void roottask_heap_note_nomem(const char *what, size_t want)
{
    static atomic_uint nfail;
    struct roottask_heap_stats hs;
    struct __rtmux_mallocstats ms;
    unsigned n = root_warn_due(&nfail);

    if (!n)
        return;
    roottask_heap_stats(&hs);
    (void) __rtmux_mallocstats(&ms);
    root_warn(
        "nomem: %s failed for %zu bytes (failure %u): arena %zu used %zu "
        "free extents %u/%zu largest %zu live %llu peak %llu\n",
        what, want, n, hs.arena, hs.used, hs.extents, hs.extent_bytes,
        hs.largest, ms.bytes_in_use, ms.bytes_peak);
}
