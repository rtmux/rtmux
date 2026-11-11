/*
 * pthread_condattr_getclock() — get the condition variable clock attribute.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <time.h>
#include <pthread.h>

int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *id)
{
    if (!attr || !id)
        return EINVAL;

    *id = (clockid_t) attr->clockid;
    return EOK;
}
