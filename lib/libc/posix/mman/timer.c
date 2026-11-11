/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include <sys/rtmux-core.h>
#include "time-util.h"

static bool itimerspec_valid(const struct itimerspec *spec)
{
    return spec && TIMESPEC_VALID(&spec->it_value) &&
           TIMESPEC_VALID(&spec->it_interval) && spec->it_value.tv_sec >= 0 &&
           spec->it_interval.tv_sec >= 0;
}

static void itimerspec_to_itimer(const struct itimerspec *src,
                                 struct _itimer *dst)
{
    dst->nsec = timespec2nsec(&src->it_value);
    dst->interval_nsec = timespec2nsec(&src->it_interval);
}

static void itimer_to_itimerspec(const struct _itimer *src,
                                 struct itimerspec *dst)
{
    __rtmux_nsec2timespec(&dst->it_value, src->nsec);
    __rtmux_nsec2timespec(&dst->it_interval, src->interval_nsec);
}

int timer_create(clockid_t clock_id, struct sigevent *evp, timer_t *timerid)
{
    int id;

    if (!evp || !timerid) {
        errno = EINVAL;
        return -1;
    }

    id = __timer_create(clock_id, evp);
    if (id == -1)
        return -1;

    *timerid = (timer_t) id;
    return 0;
}

int timer_delete(timer_t timerid)
{
    return __timer_destroy(timerid);
}

int timer_getoverrun(timer_t timerid)
{
    struct _timer_info info;

    if (__timer_info(0, timerid, 0, &info) == -1)
        return -1;

    return (int) info.overruns;
}

int timer_gettime(timer_t timerid, struct itimerspec *value)
{
    struct _timer_info info;

    if (!value) {
        errno = EINVAL;
        return -1;
    }

    if (__timer_info(0, timerid, 0, &info) == -1)
        return -1;

    itimer_to_itimerspec(&info.otime, value);
    __rtmux_nsec2timespec(&value->it_interval, info.itime.interval_nsec);
    return 0;
}

int timer_settime(timer_t timerid,
                  int flags,
                  const struct itimerspec *value,
                  struct itimerspec *ovalue)
{
    struct _itimer itime;
    struct _itimer otime;

    if ((flags & ~TIMER_ABSTIME) || !itimerspec_valid(value)) {
        errno = EINVAL;
        return -1;
    }

    itimerspec_to_itimer(value, &itime);
    if (__timer_settime(timerid, flags, &itime, ovalue ? &otime : NULL) == -1)
        return -1;

    if (ovalue)
        itimer_to_itimerspec(&otime, ovalue);
    return 0;
}
