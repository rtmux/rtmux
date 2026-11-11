/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * compiler feature helpers
 *
 * RTMux targets GCC and Clang-family compilers on RV64 LP64 and exposes its
 * POSIX baseline directly instead of carrying application-selected visibility
 * profiles.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_FEATURES_H
#define _SYS_FEATURES_H

/* Macro to test GNU C compatibility. Clang also defines __GNUC__. */
#ifndef __GNUC_PREREQ
#if defined __GNUC__ && defined __GNUC_MINOR__
#define __GNUC_PREREQ(maj, min) \
    ((__GNUC__ << 16) + __GNUC_MINOR__ >= ((maj) << 16) + (min))
#else
#define __GNUC_PREREQ(maj, min) 0
#endif
#endif

#if _FORTIFY_SOURCE > 0 && !defined(__cplusplus) && \
    (__OPTIMIZE__ > 0 || defined(__clang__)) && __GNUC_PREREQ(4, 1)
#if _FORTIFY_SOURCE > 1
#define __SSP_FORTIFY_LEVEL 2
#else
#define __SSP_FORTIFY_LEVEL 1
#endif
#else
#define __SSP_FORTIFY_LEVEL 0
#endif

#endif /* _SYS_FEATURES_H */
