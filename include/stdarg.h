/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * ISO C99 variable argument declarations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _STDARG_H
#define _STDARG_H

typedef __builtin_va_list va_list;
#define va_start(__p1, __p2) __builtin_va_start(__p1, __p2)
#define va_arg(__p1, __p2) __builtin_va_arg(__p1, __p2)
#define va_end(__p1) __builtin_va_end(__p1)
#define va_copy(__d, __s) __builtin_va_copy(__d, __s)

#endif
