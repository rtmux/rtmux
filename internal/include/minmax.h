/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _MINMAX_H
#define _MINMAX_H

#include <stddef.h>

/* Type-explicit min/max inline functions. Prefer these over the generic macros
 * below — they make the operand types visible at each call site and avoid
 * implicit sign/width conversions.
 */

static inline int min_int(int a, int b)
{
    return a < b ? a : b;
}
static inline int max_int(int a, int b)
{
    return a > b ? a : b;
}

static inline unsigned min_uint(unsigned a, unsigned b)
{
    return a < b ? a : b;
}
static inline unsigned max_uint(unsigned a, unsigned b)
{
    return a > b ? a : b;
}

static inline long min_long(long a, long b)
{
    return a < b ? a : b;
}
static inline long max_long(long a, long b)
{
    return a > b ? a : b;
}

static inline unsigned long min_ulong(unsigned long a, unsigned long b)
{
    return a < b ? a : b;
}
static inline unsigned long max_ulong(unsigned long a, unsigned long b)
{
    return a > b ? a : b;
}

static inline size_t min_size(size_t a, size_t b)
{
    return a < b ? a : b;
}
static inline size_t max_size(size_t a, size_t b)
{
    return a > b ? a : b;
}

/* Generic min/max macros — kept for call sites where both arguments already
 * have the same type. New code should use the typed variants.
 */
#if !defined(__max)
#define __max(a, b)             \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(a) _b = (b); \
        _a > _b ? _a : _b;      \
    })
#endif
#if !defined(max)
#define max(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(a) _b = (b); \
        _a > _b ? _a : _b;      \
    })
#endif
#if !defined(__min)
#define __min(a, b)             \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(a) _b = (b); \
        _a < _b ? _a : _b;      \
    })
#endif
#if !defined(min)
#define min(a, b)               \
    ({                          \
        __typeof__(a) _a = (a); \
        __typeof__(a) _b = (b); \
        _a < _b ? _a : _b;      \
    })
#endif

#endif
