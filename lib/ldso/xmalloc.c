/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "dynlink.h"
#include "printf.h"
#include "malloc.h"
#include "libc.h"

void *xcalloc(size_t number, size_t size)
{
    void *p;

    p = __crt_calloc(number, size);
    if (!p) {
        ldso_fdputstr(STDERR_FILENO, "Out of memory\n");
        _exit(1);
    }
    return (p);
}

void *xmalloc(size_t size)
{
    void *p;

    p = __crt_malloc(size);
    if (!p) {
        ldso_fdputstr(STDERR_FILENO, "Out of memory\n");
        _exit(1);
    }
    return (p);
}

char *xstrdup(const char *str)
{
    char *copy;
    size_t len;

    len = strlen(str) + 1;
    copy = xmalloc(len);
    memcpy(copy, str, len);
    return (copy);
}

void *xmalloc_aligned(size_t size, size_t align, size_t offset)
{
    void *res;

    offset &= align - 1;
    if (align < sizeof(void *))
        align = sizeof(void *);

    res = __crt_aligned_alloc_offset(align, size, offset);
    if (!res) {
        ldso_fdputstr(STDERR_FILENO, "Out of memory\n");
        _exit(1);
    }
    return (res);
}
