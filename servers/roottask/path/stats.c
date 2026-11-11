/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux namespace measurement counters.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "path.h"
#include <stdatomic.h>
#include <stdio.h>

static atomic_ullong namespace_total_roundtrips;
static atomic_ullong namespace_op_roundtrips[NAMESPACE_RT_NOPS];
static atomic_ullong namespace_cache_hits;
static atomic_ullong namespace_cache_misses;
static atomic_ullong namespace_cache_uncacheable_bypasses;
static atomic_ullong namespace_version;

static const char *const namespace_op_names[NAMESPACE_RT_NOPS] = {
    "open", "readlink", "unlink", "chdir", "chroot", "resolve", "root_resolve",
};

void namespace_stats_roundtrip(enum namespace_roundtrip_op op)
{
    atomic_fetch_add_explicit(&namespace_total_roundtrips, 1,
                              memory_order_relaxed);
    if ((unsigned) op < NAMESPACE_RT_NOPS)
        atomic_fetch_add_explicit(&namespace_op_roundtrips[op], 1,
                                  memory_order_relaxed);
}

void namespace_stats_lookup(int uncacheable)
{
    if (uncacheable)
        atomic_fetch_add_explicit(&namespace_cache_uncacheable_bypasses, 1,
                                  memory_order_relaxed);
    else
        atomic_fetch_add_explicit(&namespace_cache_misses, 1,
                                  memory_order_relaxed);
}

void namespace_version_bump(void)
{
    atomic_fetch_add_explicit(&namespace_version, 1, memory_order_acq_rel);
}

unsigned long long namespace_version_read(void)
{
    return atomic_load_explicit(&namespace_version, memory_order_acquire);
}

size_t namespace_stats_snapshot(char *buf, size_t cap)
{
    int n;
    size_t used = 0;

    if (cap == 0)
        return 0;

    n = snprintf(buf, cap, "total %llu\n",
                 atomic_load_explicit(&namespace_total_roundtrips,
                                      memory_order_relaxed));
    if (n < 0)
        return 0;
    if ((size_t) n >= cap)
        return cap - 1;
    used = (size_t) n;

    for (unsigned i = 0; i < NAMESPACE_RT_NOPS; i++) {
        unsigned long long v = atomic_load_explicit(&namespace_op_roundtrips[i],
                                                    memory_order_relaxed);

        n = snprintf(buf + used, cap - used, "%s %llu\n", namespace_op_names[i],
                     v);
        if (n < 0)
            break;
        if ((size_t) n >= cap - used)
            return cap - 1;
        used += (size_t) n;
    }
    n = snprintf(
        buf + used, cap - used,
        "cache_hits %llu\n"
        "cache_misses %llu\n"
        "cache_uncacheable_bypasses %llu\n"
        "namespace_version %llu\n",
        atomic_load_explicit(&namespace_cache_hits, memory_order_relaxed),
        atomic_load_explicit(&namespace_cache_misses, memory_order_relaxed),
        atomic_load_explicit(&namespace_cache_uncacheable_bypasses,
                             memory_order_relaxed),
        atomic_load_explicit(&namespace_version, memory_order_acquire));
    if (n < 0)
        return used;
    if ((size_t) n >= cap - used)
        return cap - 1;
    used += (size_t) n;
    return used;
}
