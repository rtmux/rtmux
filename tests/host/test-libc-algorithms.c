/*
 * Host-side run of the guest libc suites against the RTMux sources.
 *
 * The suites in tests/userland that exercise libc code with no system
 * dependency are built here unchanged, with the shared reporting hooks
 * (tests/userland/report.c), and linked against the RTMux implementation
 * instead of the host libc's, so they also run under the host sanitizers.
 * libc-host-shim.h is force-included into every source; this file supplies the
 * counting allocator behind it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdlib.h>

#include "test.h"

/* The real allocator, for the shim's implementation below. */
#undef malloc
#undef calloc
#undef free

static struct host_alloc_stats stats;
static long fail_after = -1;

/* Each block carries its size in front, so free can keep live exact. */
union host_hdr {
    size_t size;
    max_align_t align;
};

static int host_fail(void)
{
    if (fail_after < 0)
        return 0;
    if (fail_after == 0)
        return 1;
    fail_after--;
    return 0;
}

void host_alloc_snapshot(struct host_alloc_stats *s)
{
    *s = stats;
}

void host_alloc_fail_after(long n)
{
    fail_after = n;
}

void *host_malloc(size_t size)
{
    union host_hdr *h;

    if (host_fail() || size > SIZE_MAX - sizeof *h || !(h = malloc(sizeof *h + size)))
        return NULL;
    h->size = size;
    stats.calls++;
    stats.bytes += size;
    stats.live += size;
    return h + 1;
}

void *host_calloc(size_t n, size_t size)
{
    void *p;

    if (size && n > SIZE_MAX / size)
        return NULL;
    p = host_malloc(n * size);
    if (p)
        memset(p, 0, n * size);
    return p;
}

void host_free(void *p)
{
    union host_hdr *h = p;

    if (!p)
        return;
    h--;
    stats.live -= h->size;
    free(h);
}

int main(void)
{
    test_qsort();
    test_getopt();
    test_env();
    tc_summary(&suite_counts);
    return tc_status(&suite_counts, 1) ? EXIT_FAILURE : EXIT_SUCCESS;
}
