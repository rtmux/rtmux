/*
 * pthread_attr_getstacksize() — get the stack size attribute.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *stacksize)
{
    if (!attr || !stacksize)
        return EINVAL;

    *stacksize = attr->stacksize;
    return EOK;
}
