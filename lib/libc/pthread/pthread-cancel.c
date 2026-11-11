/*
 * pthread_cancel().
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <pthread.h>
#include <sys/rtmux-core.h>
#include "cpucfg.h"

static void pthread_cancel_stub(void)
{
    pthread_exit(PTHREAD_CANCELED);
}

int pthread_cancel(pthread_t thread)
{
    return __rtmux_pthread_ret(
        thread_cancel_r((int) thread, pthread_cancel_stub));
}
