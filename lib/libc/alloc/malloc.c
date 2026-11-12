/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * RTMux libc — malloc/free/calloc/realloc wrappers
 *
 * Thin wrappers around the FreeBSD-derived __crt_* allocator. This keeps the
 * POSIX namespace separate from the allocator internals.
 */

#include <stddef.h>
#include <errno.h>
#include "malloc-core.h"

void *malloc(size_t size)
{
    return __crt_malloc(size);
}

void free(void *ptr)
{
    __crt_free(ptr);
}

void *calloc(size_t nmemb, size_t size)
{
    return __crt_calloc(nmemb, size);
}

void *realloc(void *ptr, size_t size)
{
    return __crt_realloc(ptr, size);
}

int posix_memalign(void **memptr, size_t alignment, size_t size)
{
    void *p;

    if (!memptr)
        return EINVAL;

    /* alignment must be a power of two and a multiple of sizeof(void *) */
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 ||
        (alignment % sizeof(void *)) != 0)
        return EINVAL;
    p = __crt_aligned_alloc_offset(alignment, size, 0);
    /* size == 0 may legitimately return NULL; that's success, not ENOMEM. */
    if (!p && size != 0)
        return ENOMEM;
    *memptr = p;
    return 0;
}
