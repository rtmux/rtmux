/*
 * POSIX scheduler wrappers.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include "time-util.h"

static bool sched_current_only(pid_t pid)
{
    if (pid == 0 || pid == getpid())
        return true;

    errno = ENOSYS;
    return false;
}

int sched_getparam(pid_t pid, struct sched_param *param)
{
    if (!param) {
        errno = EINVAL;
        return -1;
    }
    if (!sched_current_only(pid))
        return -1;

    if (__sched_get(0, 0, param) == -1)
        return -1;
    return 0;
}

int sched_getscheduler(pid_t pid)
{
    struct sched_param param;

    if (!sched_current_only(pid))
        return -1;

    return __sched_get(0, 0, &param);
}

int sched_setparam(pid_t pid, const struct sched_param *param)
{
    if (!param) {
        errno = EINVAL;
        return -1;
    }
    if (!sched_current_only(pid))
        return -1;

    return __sched_set(0, 0, RTMUX_SCHED_NOCHANGE, param);
}

int sched_setscheduler(pid_t pid, int policy, const struct sched_param *param)
{
    if (!param) {
        errno = EINVAL;
        return -1;
    }
    if (!sched_current_only(pid))
        return -1;

    return __sched_set(0, 0, policy, param);
}

int sched_rr_get_interval(pid_t pid, struct timespec *t)
{
    struct _sched_info info;

    if (!t) {
        errno = EINVAL;
        return -1;
    }
    if (!sched_current_only(pid))
        return -1;
    if (__sched_info(0, SCHED_RR, &info) == -1)
        return -1;

    __rtmux_nsec2timespec(t, info.interval);
    return 0;
}

int sched_get_priority_min(int alg)
{
    struct _sched_info info;

    if (__sched_info(0, alg, &info) == -1)
        return -1;
    return info.priority_min;
}

int sched_get_priority_max(int alg)
{
    struct _sched_info info;

    if (__sched_info(0, alg, &info) == -1)
        return -1;
    return info.priority_max;
}
