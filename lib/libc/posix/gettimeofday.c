/*
 * gettimeofday — get current time (POSIX)
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/time.h>
#include <sys/rtmux-core.h>
#include <time.h>
#include <errno.h>

int gettimeofday(struct timeval *tv, void *tz)
{
    uint64_t nsec;

    (void) tz;
    if (!tv) {
        errno = EINVAL;
        return -1;
    }
    if (__clock_time(CLOCK_REALTIME, NULL, &nsec) == -1)
        return -1;

    tv->tv_sec = (time_t) (nsec / 1000000000ULL);
    tv->tv_usec = (suseconds_t) ((nsec % 1000000000ULL) / 1000);
    return 0;
}
