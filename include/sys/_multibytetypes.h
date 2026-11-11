/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Multibyte character type definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_MULTIBYTETYPES_H
#define _SYS_MULTIBYTETYPES_H

#include <stddef.h>

#if !defined(__WCHAR_TYPE__) || !defined(__WINT_TYPE__)
#error __WCHAR_TYPE__ and __WINT_TYPE__ must be defined by the compiler stddef.h
#endif

typedef __WCHAR_TYPE__ wchar_t;
typedef __WINT_TYPE__ wint_t;

/* Conversion state information */
typedef struct {
    int __count;
    union {
        wint_t __wch;
        unsigned char __wchb[4];
    } __value; /* Value so far */
} mb_state_t;

typedef mb_state_t _mbstate_t;

/* Iconv descriptor type */
typedef void *iconv_t;

#endif
