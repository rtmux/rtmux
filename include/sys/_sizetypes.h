/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Size-related POSIX type definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_SIZETYPES_T
#define _SYS_SIZETYPES_T

#include <stddef.h>

#ifndef __SIZE_TYPE__
#error __SIZE_TYPE__ must be defined by the compiler stddef.h
#endif

typedef __SIZE_TYPE__ size_t;
/* Define ssize_t based on size_t by flipping "signed" and "unsigned" */
#define unsigned signed
typedef __SIZE_TYPE__ ssize_t;
#undef unsigned

#endif
