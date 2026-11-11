/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * ISO C99 diagnostics declarations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/cdefs.h>

#if !defined(__cplusplus) && \
    (!defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L)
#ifndef static_assert
#define static_assert _Static_assert
#endif
#endif

#undef assert

#ifdef NDEBUG
#define assert(x) ((void) 0)
#else

#define assert(__expr) \
    ((__expr) ? (void) 0 : __assert_fail(#__expr, __FILE__, __LINE__, __func__))

__BEGIN_DECLS
extern void __assert_fail(const char *__expr,
                          const char *__file,
                          unsigned __line,
                          const char *__func);
__END_DECLS

#endif
