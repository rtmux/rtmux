/*
 * SPDX-License-Identifier: Apache-2.0
 * Miscellaneous libc support for kernel-mode roottask.
 */

#include <time.h>
#include <stdint.h>
#include <sys/rtmux-core.h>

/* time() — get current calendar time in seconds since the Epoch. Uses
 * __clock_time(CLOCK_REALTIME) which reads syspage qtime (nsec +
 * nsec_tod_adjust).
 */
time_t time(time_t *tloc)
{
    uint64_t nsec;

    if (__clock_time(CLOCK_REALTIME, NULL, &nsec) == -1) {
        if (tloc)
            *tloc = (time_t) -1;
        return (time_t) -1;
    }

    time_t sec = (time_t) (nsec / 1000000000ULL);
    if (tloc)
        *tloc = sec;
    return sec;
}
