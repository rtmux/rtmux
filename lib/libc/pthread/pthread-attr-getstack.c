/*
 * pthread_attr_getstack() — get the stack address and size attributes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_getstack(const pthread_attr_t *attr,
                          void **stackaddr,
                          size_t *stacksize)
{
    if (!attr || !stackaddr || !stacksize)
        return EINVAL;

    *stackaddr = attr->stackaddr;
    *stacksize = attr->stacksize;
    return EOK;
}
