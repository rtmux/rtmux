/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* The state of roottask's fixed malloc arena (sys/heap.c). */

#ifndef _ROOTTASK_HEAP_STATS_H
#define _ROOTTASK_HEAP_STATS_H

#include <stddef.h>

struct roottask_heap_stats {
    size_t arena;        /* size of the arena */
    size_t used;         /* bytes below the bump pointer */
    size_t extent_bytes; /* bytes in free extents below it */
    size_t largest;      /* largest block a request could still get */
    size_t dropped;      /* bytes lost when the extent table was full */
    unsigned extents;    /* free extents */
};

void roottask_heap_stats(struct roottask_heap_stats *st);

/* Log, rate limited, that a roottask allocation of `want` bytes for `what`
 * failed, with the arena and malloc counters at that moment.
 */
void roottask_heap_note_nomem(const char *what, size_t want);

#endif
