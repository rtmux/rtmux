/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef __COMPILER_LP64_H_INCLUDED
#define __COMPILER_LP64_H_INCLUDED

#if !defined(__clang__) && (!defined(__GNUC__) || __GNUC__ < 12)
#error GCC 12+ or a Clang-family compiler required
#endif
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 199901L
#error C99 or newer required
#endif

#if __has_attribute(__fallthrough__)
#define fallthrough __attribute__((__fallthrough__))
#else
#define fallthrough \
    do {            \
    } while (0) /* fallthrough */
#endif

#ifndef __PLATFORM_H_INCLUDED
#error __FILE__ should not be included directly.
#endif

#ifdef __CHAR_UNSIGNED__
#undef __CHAR_SIGNED__
#else
#ifndef __CHAR_SIGNED__
#define __CHAR_SIGNED__
#endif
#endif

#ifdef __WCHAR_TYPE__
#undef __WCHAR_T
#define __WCHAR_T __WCHAR_TYPE__
#endif

#ifdef __WCHAR_MAX__
#undef _WCMAX
#define _WCMAX __WCHAR_MAX__
#endif

/* RTMux targets little-endian RV64 only. */

#ifndef _NO_RETURN
#define _NO_RETURN(fun) void fun __attribute__((__noreturn__))
#endif

#ifndef __CDEFS_H_INCLUDED
#include <sys/cdefs.h>
#endif

/* Use LP64 model */
#define __INT_BITS__ 32
#define __PTR_BITS__ 64
#define __LONG_BITS__ 64

#define __LONGDOUBLE_BITS__ 64

typedef signed char int8_t;
typedef unsigned char uint8_t;
typedef signed short int16_t;
typedef unsigned short uint16_t;
typedef signed int int32_t;
typedef unsigned int uint32_t;
typedef signed long int64_t;
typedef unsigned long uint64_t;

typedef signed long intptr_t;
typedef unsigned long uintptr_t;

typedef int8_t int_least8_t;
typedef uint8_t uint_least8_t;
typedef int8_t int_fast8_t;
typedef uint8_t uint_fast8_t;
typedef int16_t int_least16_t;
typedef uint16_t uint_least16_t;
typedef int16_t int_fast16_t;
typedef uint16_t uint_fast16_t;
typedef int32_t int_least32_t;
typedef uint32_t uint_least32_t;
typedef int32_t int_fast32_t;
typedef uint32_t uint_fast32_t;
typedef int64_t int_least64_t;
typedef uint64_t uint_least64_t;
typedef int64_t int_fast64_t;
typedef uint64_t uint_fast64_t;
typedef int64_t intmax_t;
typedef uint64_t uintmax_t;

/* Override compiler-provided fallback typedef guards so RTMux owns these types.
 */
#define _GCC_PTRDIFF_T
#define _GCC_SIZE_T
#define _GCC_WCHAR_T

/* -ansi defines __STRICT_ANSI__ -traditional undefines __STDC__ and
 * __STDC_VERSION__
 */
#if defined(__STDC__) && !defined(__STRICT_ANSI__)
#define __EXT
#endif

#endif
