/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Offset-related POSIX type definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_OFFSETTYPES_H
#define _SYS_OFFSETTYPES_H

#include <stdint.h>

/* On LP64, POSIX file offsets are 64-bit. */
typedef long off_t;

typedef long long off64_t;

/* File position */
typedef off_t fpos_t;

#endif
