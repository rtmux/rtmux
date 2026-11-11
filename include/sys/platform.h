/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux targets little-endian RV64 LP64 only. Little-endian only. No 32-bit, no
 * big-endian.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __PLATFORM_H_INCLUDED
#define __PLATFORM_H_INCLUDED

#include <arch/platform.h>
#include <sys/compiler-lp64.h>

#if !defined(__FLEXARY)
#define __FLEXARY(__type, __name) __type __name[1]
#endif

/* Data model: LP64 only. int=32, long=64, ptr=64. */
#if __INT_BITS__ == 32 && __LONG_BITS__ == 64 && __PTR_BITS__ == 64
#define __LP64__ 1
#define __LP64 1
#define _LP64 1
#else
#error RTMux requires LP64 (int=32, long=64, ptr=64)
#endif

#endif
