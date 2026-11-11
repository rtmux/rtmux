/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sys/rtmux-core.h>
#include "cpucfg.h"
#include "signal-state.h"

int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex)
{
    libc_waiter_t w;
    int ret;

    if ((mutex->owner & ~RTMUX_SYNC_WAITING) != LIBC_TLS()->__owner) {
        return EPERM;
    }
    __libc_wait_enter(&w);
    ret = sync_condvar_wait_r((sync_t *) cond, (sync_t *) mutex);
    __libc_wait_exit(&w);
    /* _r variant returns -errno on error, 0 on success */
    if (ret == -EINTR || ret == EOK)
        return EOK;
    return __rtmux_pthread_ret(ret);
}
