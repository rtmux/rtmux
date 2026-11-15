/*
 * Host build shim for the libc suites in tests/userland (LIBC_HOST_TEST).
 *
 * Force-included into every source of tests/host/test-libc-algorithms.c, so
 * the RTMux libc sources under test build against the host headers and their
 * allocations go through a counting allocator that can also be told to fail.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIBC_HOST_SHIM_H
#define LIBC_HOST_SHIM_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/cdefs.h>

#ifndef __hidden
#define __hidden
#endif

struct host_alloc_stats {
    unsigned long calls; /* successful allocations so far */
    unsigned long bytes; /* bytes requested by those allocations */
    unsigned long live;  /* bytes allocated and not yet freed */
};

void host_alloc_snapshot(struct host_alloc_stats *s);
/* Let n more allocations succeed, then fail every one; n < 0 never fails. */
void host_alloc_fail_after(long n);

void *host_malloc(size_t size);
void *host_calloc(size_t n, size_t size);
void host_free(void *p);

#ifndef LIBC_HOST_SHIM_IMPL
#define malloc host_malloc
#define calloc host_calloc
#define free host_free
#endif

#endif /* LIBC_HOST_SHIM_H */
