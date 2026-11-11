/*
 * pthread cancellation state attributes.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>
#include <atomic.h>
#include "cpucfg.h"

static int valid_cancel_state(int state)
{
    return state == PTHREAD_CANCEL_ENABLE || state == PTHREAD_CANCEL_DISABLE;
}

static int valid_cancel_type(int type)
{
    return type == PTHREAD_CANCEL_DEFERRED ||
           type == PTHREAD_CANCEL_ASYNCHRONOUS;
}

int pthread_setcancelstate(int state, int *oldstate)
{
    volatile unsigned *flags = &LIBC_TLS()->__flags;
    unsigned old;

    if (!valid_cancel_state(state))
        return EINVAL;

    if (state == PTHREAD_CANCEL_DISABLE) {
        old = atomic_or_value(flags, PTHREAD_CANCEL_DISABLE);
    } else {
        old = atomic_clr_value(flags, PTHREAD_CANCEL_DISABLE);
        pthread_testcancel();
    }
    if (oldstate)
        *oldstate = (int) (old & PTHREAD_CSTATE_MASK);
    return EOK;
}

int pthread_setcanceltype(int type, int *oldtype)
{
    volatile unsigned *flags = &LIBC_TLS()->__flags;
    unsigned old;

    if (!valid_cancel_type(type))
        return EINVAL;

    if (type == PTHREAD_CANCEL_ASYNCHRONOUS) {
        old = atomic_or_value(flags, PTHREAD_CANCEL_ASYNCHRONOUS);
        pthread_testcancel();
    } else {
        old = atomic_clr_value(flags, PTHREAD_CANCEL_ASYNCHRONOUS);
    }
    if (oldtype)
        *oldtype = (int) (old & PTHREAD_CTYPE_MASK);
    return EOK;
}
