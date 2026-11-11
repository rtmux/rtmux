/*
 * pthread scheduling wrappers.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <sys/rtmux-core.h>

int pthread_getschedparam(const pthread_t thread,
                          int *policy,
                          struct sched_param *param)
{
    if (!policy || !param)
        return EINVAL;

    int rc = __sched_get(0, thread, param);
    if (rc == -1)
        return errno;

    *policy = rc;
    return EOK;
}

int pthread_setschedparam(pthread_t thread,
                          int policy,
                          const struct sched_param *param)
{
    if (!param)
        return EINVAL;

    if (__sched_set(0, thread, policy, param) == -1)
        return errno;
    return EOK;
}

int pthread_setschedprio(pthread_t thread, int prio)
{
    struct sched_param param = {.sched_priority = prio};

    if (__sched_set(0, thread, RTMUX_SCHED_SETPRIO, &param) == -1)
        return errno;
    return EOK;
}
