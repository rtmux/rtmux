/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <time.h>
#include <errno.h>
#include <sys/rtmux-core.h>
#include "time-util.h"
#include "signal-state.h"

int nanosleep(const struct timespec *rqtp, struct timespec *rmtp)
{
    uint64_t t, otime;
    libc_waiter_t w;
    int r;

    if (!rqtp || !TIMESPEC_VALID(rqtp) || rqtp->tv_sec < 0) {
        errno = EINVAL;
        return -1;
    }
    t = (uint64_t) rqtp->tv_sec * 1000000000ULL + (uint64_t) rqtp->tv_nsec;
    __libc_wait_enter(&w);
    r = __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_NANOSLEEP, NULL, &t,
                          rmtp ? &otime : NULL);
    __libc_wait_exit(&w);
    if (r != EOK) {
        r = r < 0 ? -r : r;
        if (rmtp) {
            rmtp->tv_sec = (time_t) (otime / 1000000000ULL);
            rmtp->tv_nsec = (long) (otime % 1000000000ULL);
        }
        errno = r;
        return -1;
    }
    return 0;
}

int clock_nanosleep(clockid_t clock_id,
                    int flags,
                    const struct timespec *rqtp,
                    struct timespec *rmtp)
{
    uint64_t timeout;
    uint64_t remaining;
    libc_waiter_t w;
    int ret;

    if ((flags & ~TIMER_ABSTIME) || !rqtp || !TIMESPEC_VALID(rqtp) ||
        rqtp->tv_sec < 0)
        return EINVAL;

    timeout = timespec2nsec(rqtp);
    __libc_wait_enter(&w);
    ret = __timer_timeout_r(clock_id, flags | RTMUX_TIMEOUT_NANOSLEEP, NULL,
                            &timeout, rmtp ? &remaining : NULL);
    __libc_wait_exit(&w);
    if (ret == EOK)
        return 0;

    if (rmtp && !(flags & TIMER_ABSTIME))
        __rtmux_nsec2timespec(rmtp, remaining);
    return ret < 0 ? -ret : ret;
}
