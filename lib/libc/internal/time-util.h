/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef LIBC_TIME_UTIL_H
#define LIBC_TIME_UTIL_H

#include <stdint.h>
#include <time.h>

static inline void __rtmux_nsec2timespec(struct timespec *ts, uint64_t nsec)
{
    ts->tv_sec = (time_t) (nsec / 1000000000ULL);
    ts->tv_nsec = (long) (nsec % 1000000000ULL);
}

#endif
