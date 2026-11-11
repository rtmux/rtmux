/*
 * pthread_testcancel().
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <pthread.h>
#include "cpucfg.h"

#undef pthread_testcancel
void pthread_testcancel(void)
{
    unsigned flags = LIBC_TLS()->__flags;

    if ((flags & (PTHREAD_CSTATE_MASK | PTHREAD_CANCEL_PENDING)) ==
        (PTHREAD_CANCEL_ENABLE | PTHREAD_CANCEL_PENDING))
        pthread_exit(PTHREAD_CANCELED);
}
