/*
 * pthread_attr_setstackaddr() — set the stack address attribute.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_setstackaddr(pthread_attr_t *attr, void *stackaddr)
{
    if (!attr)
        return EINVAL;

    attr->stackaddr = stackaddr;
    return EOK;
}
