/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <time.h>
#include <sys/rtmux-core.h>
#include <sys/syspage.h>
#include "time-util.h"

int clock_gettime(clockid_t clock_id, struct timespec *tp)
{
    uint64_t nsec;

    if (!tp) {
        errno = EINVAL;
        return -1;
    }
    if (__clock_time(clock_id, NULL, &nsec) == -1)
        return -1;
    __rtmux_nsec2timespec(tp, nsec);
    return 0;
}

int clock_settime(clockid_t clock_id, const struct timespec *tp)
{
    uint64_t nsec;

    if (!tp || tp->tv_nsec < 0 || tp->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }
    nsec = timespec2nsec(tp);
    return __clock_time(clock_id, &nsec, NULL);
}

int clock_getres(clockid_t clock_id, struct timespec *res)
{
    struct _clockperiod period = {
        .nsec = 0,
        .fract = 0,
    };
    const struct qtime_entry *qt;

    switch (clock_id) {
    case CLOCK_THREAD_CPUTIME_ID:
        break;
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_PROCESS_CPUTIME_ID:
        clock_id = CLOCK_REALTIME;
        break;
    default:
        /* Anything else is a CPU-time clock from pthread_getcpuclockid or
         * clock_getcpuclockid, or not a clock: the kernel knows which (a read
         * that asks for no value only validates the ID). A process's clock has
         * no thread number in its low 16 bits.
         */
        if (clock_id > INT_MAX || __clock_time(clock_id, NULL, NULL) == -1) {
            errno = EINVAL;
            return -1;
        }
        clock_id =
            (clock_id & 0xffffU) ? CLOCK_THREAD_CPUTIME_ID : CLOCK_REALTIME;
        break;
    }
    if (!res)
        return 0;

    /* A thread's CPU-time clock is read from the free-running timebase
     * (snap_time): one timebase count when that is in nanoseconds (always on
     * RISC-V), one tick otherwise. The process CPU-time clock is the banked
     * total, to which a thread running on another hart adds at its tick, so
     * like the time-of-day clocks it resolves one tick.
     */
    if (clock_id == CLOCK_THREAD_CPUTIME_ID &&
        (qt = SYSPAGE_ENTRY(qtime))->timer_scale == -9) {
        __rtmux_nsec2timespec(res, qt->timer_rate);
        return 0;
    }
    if (__clock_period(CLOCK_REALTIME, NULL, &period, 0) == -1)
        return -1;
    __rtmux_nsec2timespec(res, period.nsec);
    return 0;
}

int clock_getcpuclockid(pid_t pid, clockid_t *clock_idp)
{
    int id;

    if (!clock_idp) {
        errno = EINVAL;
        return EINVAL;
    }
    id = __clock_id(pid, 0);
    if (id == -1)
        return errno;
    *clock_idp = (clockid_t) id;
    return 0;
}

int pthread_getcpuclockid(pthread_t id, clockid_t *clock_idp)
{
    int clkid;

    if (!clock_idp)
        return EINVAL;
    clkid = __clock_id(0, id);
    if (clkid == -1)
        return errno;
    *clock_idp = (clockid_t) clkid;
    return 0;
}
