/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_COMPILER_H
#define _SYS_COMPILER_H

#ifdef __GNUC__
#define ALWAYS_INLINE static __attribute__((always_inline)) inline
#else
#define ALWAYS_INLINE static inline
#endif

#endif /* _SYS_COMPILER_H */
