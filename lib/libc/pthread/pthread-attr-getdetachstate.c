/*
 * pthread_attr_getdetachstate() — get the detach state attribute.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *detachstate)
{
    if (!attr || !detachstate)
        return EINVAL;

    *detachstate = attr->flags & PTHREAD_CREATE_DETACHED
                       ? PTHREAD_CREATE_DETACHED
                       : PTHREAD_CREATE_JOINABLE;
    return EOK;
}
