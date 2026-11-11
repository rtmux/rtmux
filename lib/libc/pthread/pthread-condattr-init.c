/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h> /* for memset */
#include <time.h>
#include <pthread.h>
#include <sys/rtmux-core.h>

int pthread_condattr_init(pthread_condattr_t *attr)
{
    if (!attr)
        return EINVAL;

    memset(attr, 0, sizeof *attr);
    attr->flags = RTMUX_ATTR_COND;
    attr->protocol = PTHREAD_PRIO_NONE;
    attr->clockid = CLOCK_REALTIME;
    return EOK;
}
