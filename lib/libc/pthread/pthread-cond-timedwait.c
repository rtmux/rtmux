/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <inttypes.h>
#include <sys/rtmux-core.h>
#include "cpucfg.h"
#include "signal-state.h"

int pthread_cond_timedwait(pthread_cond_t *cond,
                           pthread_mutex_t *mutex,
                           const struct timespec *abstime)
{
    libc_waiter_t w;
    int ret;
    uint64_t t;

    if ((mutex->owner & ~RTMUX_SYNC_WAITING) != LIBC_TLS()->__owner) {
        return EPERM;
    }

    if (!TIMESPEC_VALID(abstime)) {
        return EINVAL;
    }

    t = timespec2nsec(abstime);

    /* Passing a NULL for event is the same as notify = SIGEV_UNBLOCK */
    ret = __timer_timeout_r(cond->owner == RTMUX_SYNC_INITIALIZER
                                ? CLOCK_REALTIME
                                : (clockid_t) cond->count,
                            TIMER_ABSTIME | RTMUX_TIMEOUT_CONDVAR, 0, &t, 0);
    if ((ret = __rtmux_pthread_ret(ret)) != EOK) {
        return ret;
    }

    __libc_wait_enter(&w);
    ret = __rtmux_pthread_ret(
        sync_condvar_wait_r((sync_t *) cond, (sync_t *) mutex));
    __libc_wait_exit(&w);
    if (ret == EINTR) {
        ret = EOK;
    }
    return ret;
}
