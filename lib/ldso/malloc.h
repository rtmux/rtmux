/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifndef DYNLINK_MALLOC_H
#define DYNLINK_MALLOC_H

void *__crt_aligned_alloc_offset(size_t align, size_t size, size_t offset);
void *__crt_calloc(size_t num, size_t size);
void __crt_free(void *cp);
void *__crt_malloc(size_t nbytes);
void *__crt_realloc(void *cp, size_t nbytes);

extern int npagesizes;
extern size_t *pagesizes;

#endif
