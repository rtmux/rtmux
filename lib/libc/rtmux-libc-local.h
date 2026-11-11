/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * rtmux-libc-local.h — local libc build definitions.
 *
 * Included via -include rtmux-libc-local.h in the Makefile so every translation
 * unit sees the same LP64 build contract.
 */
#ifndef _RTMUX_LIBC_LOCAL_H
#define _RTMUX_LIBC_LOCAL_H

#include <sys/cdefs.h>

#define _FILE_OFFSET_BITS 64

#define LIBC_WEAK(a, b)

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/types.h>
#ifndef LONG_BIT
#if __LP64__
#define LONG_BIT 64
#else
#define LONG_BIT 32
#endif
#endif

ssize_t _readx(int fd,
               void *buff,
               size_t nbytes,
               unsigned xtype,
               void *xdata,
               size_t xdatalen) __hidden;

int _connect(int base,
             const char *path,
             mode_t mode,
             unsigned oflag,
             unsigned sflag,
             unsigned subtype,
             int testcancel,
             unsigned access,
             unsigned file_type,
             unsigned extra_type,
             unsigned extra_len,
             const void *extra,
             unsigned response_len,
             void *response,
             int *status) __hidden;

int _vopen(const char *path, int oflag, int sflag, va_list ap) __hidden;

extern const pthread_attr_t pthread_attr_default __hidden;

#endif /* _RTMUX_LIBC_LOCAL_H */
