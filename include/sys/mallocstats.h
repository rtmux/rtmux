/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* sys/mallocstats.h - the allocator's counters (see __rtmux_mallocstats). */

#ifndef _SYS_MALLOCSTATS_H_
#define _SYS_MALLOCSTATS_H_

#include <sys/cdefs.h>

/* RTMux extension: counters of this process's allocator, for tests and
 * monitoring. They count blocks taken from and returned to the allocator: a
 * block's size is its usable payload, which the allocator rounds up from the
 * request, and posix_memalign takes one block that backs the aligned span.
 * allocs and frees count successful takes and returns (a realloc that moves is
 * one of each); failed counts requests that returned NULL; bytes_peak is the
 * high-water mark of bytes_in_use. The fields are 64 bits and are read one at a
 * time, so a snapshot taken while other threads allocate need not be
 * consistent.
 *
 * bytes_peak is exact while one thread allocates, and approximate otherwise:
 * small blocks and separately mapped large blocks are counted apart, without a
 * common lock, and each raises the peak from its own total plus the other's
 * as last seen. A small and a mapped allocation that race can both miss the
 * other, so the peak can stay below the sum they reached, and so below a
 * later bytes_in_use, until another allocation raises it. Making it exact
 * would put a fence or a shared lock on every allocation.
 */
struct __rtmux_mallocstats {
    unsigned long long allocs;
    unsigned long long frees;
    unsigned long long bytes_in_use;
    unsigned long long bytes_peak;
    unsigned long long failed;
};

__BEGIN_DECLS
/* Fill *__st; returns 0, or -1 with errno EINVAL if it is NULL. */
extern int __rtmux_mallocstats(struct __rtmux_mallocstats *__st);
__END_DECLS

#endif /* _SYS_MALLOCSTATS_H_ */
