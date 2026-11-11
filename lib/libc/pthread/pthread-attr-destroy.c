/*
 * pthread_attr_destroy() — destroy a thread attribute object.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_destroy(pthread_attr_t *attr)
{
    (void) attr;
    return EOK;
}
