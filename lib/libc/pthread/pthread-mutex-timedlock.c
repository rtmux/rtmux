/*
 * pthread_mutex_timedlock().
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <sys/rtmux-core.h>
#include "cpucfg.h"
#include "pthread-ulock-debug.h"

/* timedlock never acquires the mutex itself: every success routes through
 * pthread_mutex_trylock or pthread_mutex_lock, both of which already run the
 * ulock-debug acquire hook. So there is no separate return EOK to instrument
 * here (and adding an unused __pc would trip -Wunused). The header is included
 * only to keep this file's dependency on the hook API explicit.
 */

int pthread_mutex_timedlock(pthread_mutex_t *mutex,
                            const struct timespec *abs_timeout)
{
    uint64_t timeout;
    int ret;

    ret = pthread_mutex_trylock(mutex);
    if (ret != EBUSY)
        return ret;

    if (!abs_timeout || !TIMESPEC_VALID(abs_timeout))
        return EINVAL;

    timeout = timespec2nsec(abs_timeout);
    ret = __timer_timeout_r(CLOCK_REALTIME, TIMER_ABSTIME | RTMUX_TIMEOUT_MUTEX,
                            NULL, &timeout, NULL);
    if ((ret = __rtmux_pthread_ret(ret)) != EOK)
        return ret;

    return pthread_mutex_lock(mutex);
}
