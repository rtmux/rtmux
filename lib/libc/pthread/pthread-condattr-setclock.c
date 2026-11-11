/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <time.h>
#include <pthread.h>

int pthread_condattr_setclock(pthread_condattr_t *attr, int id)
{
    if (!attr)
        return EINVAL;

    switch (id) {
    case CLOCK_REALTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_SOFTTIME:
        break;
    default:
        return EINVAL;
    }

    attr->flags |= RTMUX_ATTR_EXTRA_FLAG;
    attr->clockid = id;
    return EOK;
}
