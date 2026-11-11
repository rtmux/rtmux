/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h> /* for memset */
#include <pthread.h>

int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{
    if (!attr)
        return EINVAL;

    memset(attr, 0, sizeof *attr);
    attr->flags = RTMUX_ATTR_MUTEX;
    attr->protocol = PTHREAD_PRIO_NONE;
    return EOK;
}
