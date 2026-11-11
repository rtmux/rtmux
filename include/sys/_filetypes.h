/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * File-related POSIX type definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_FILETYPES_H
#define _SYS_FILETYPES_H

#include <stdint.h>

typedef uint32_t dev_t;
typedef uint64_t ino_t;
typedef uint32_t mode_t;

typedef int nl_item;
typedef unsigned nlink_t;

/* Block counts */
typedef uint64_t blkcnt_t;
typedef uint64_t blksize_t;
typedef uint64_t fsblkcnt_t;
typedef uint64_t fsfilcnt_t;

#endif
