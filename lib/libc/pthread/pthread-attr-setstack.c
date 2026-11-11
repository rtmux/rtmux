/*
 * pthread_attr_setstack() — set the stack address and size attributes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_setstack(pthread_attr_t *attr,
                          void *stackaddr,
                          size_t stacksize)
{
    if (!attr || (stacksize != 0 && stacksize < PTHREAD_STACK_MIN))
        return EINVAL;

    attr->stackaddr = stackaddr;
    attr->stacksize = stacksize;
    return EOK;
}
