/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* alloca.h: Allocate memory from the stack. */

#ifndef _ALLOCA_H_INCLUDED
#define _ALLOCA_H_INCLUDED

#ifndef __PLATFORM_H_INCLUDED
#include <sys/platform.h>
#endif

__BEGIN_DECLS

extern size_t __stackavail(void);

#undef alloca

#define __ALLOCA_ALIGN(s) \
    (((size_t) (s) + (sizeof(uint64_t) - 1)) & ~(sizeof(uint64_t) - 1))

#if defined(__GNUC__) || defined(__INTEL_COMPILER)
extern void *__builtin_alloca(unsigned long __size);
#define _alloca(s) __builtin_alloca(s)
#else
#error not configured for system
#endif

#define alloca(s) \
    (((__ALLOCA_ALIGN(s) + 128) < __stackavail()) ? _alloca(s) : 0)
#define __alloca(s) _alloca(s)

__END_DECLS
#endif
