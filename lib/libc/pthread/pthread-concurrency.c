/*
 * pthread concurrency hint.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>

static atomic_int pthread_concurrency;

int pthread_getconcurrency(void)
{
    return atomic_load_explicit(&pthread_concurrency, memory_order_relaxed);
}

int pthread_setconcurrency(int new_level)
{
    if (new_level < 0)
        return EINVAL;

    atomic_store_explicit(&pthread_concurrency, new_level,
                          memory_order_relaxed);
    return EOK;
}
