/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 The FreeBSD Foundation
 * Adapted for RTMux.
 */

#ifndef _RTMUX_MALLOC_CORE_H
#define _RTMUX_MALLOC_CORE_H

#include <stddef.h>
#include <sys/cdefs.h>

void *__crt_aligned_alloc_offset(size_t align,
                                 size_t size,
                                 size_t offset) __hidden;
void *__crt_calloc(size_t num, size_t size) __hidden;
void __crt_free(void *cp) __hidden;
void *__crt_malloc(size_t nbytes) __hidden;
void *__crt_realloc(void *cp, size_t nbytes) __hidden;

#endif
