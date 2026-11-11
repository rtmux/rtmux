/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Time-related POSIX type definitions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_TIMETYPES_H
#define _SYS_TIMETYPES_H

#include <stdint.h>

/* Our time_t is signed 64-bit. */
typedef int64_t time_t;
typedef uint64_t useconds_t; /* Unsigned */
typedef int64_t suseconds_t; /* Signed */
typedef int64_t sbintime_t;

/* Used to define time specifications. */
struct timespec {
    time_t tv_sec;   /* seconds */
    int64_t tv_nsec; /* and nanoseconds */
};

/* Structure returned by gettimeofday(2) system call, and used in other calls.
 */
struct timeval {
    time_t tv_sec;       /* seconds */
    suseconds_t tv_usec; /* and microseconds */
};

#endif
