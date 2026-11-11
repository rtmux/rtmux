/*
 * pthread_attr_getstackaddr() — get the stack address attribute.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_getstackaddr(const pthread_attr_t *attr, void **stackaddr)
{
    if (!attr || !stackaddr)
        return EINVAL;

    *stackaddr = attr->stackaddr;
    return EOK;
}
