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
#include "pthread-ulock-debug.h"

int(pthread_mutex_trylock)(pthread_mutex_t *mutex)
{
    const void *__attribute__((__unused__)) __pc = __builtin_return_address(0);
    unsigned long owner, id = LIBC_TLS()->__owner;
    int ret;

    if ((mutex->count & RTMUX_SYNC_PRIOCEILING) != 0) {
        /* Re-lock by the owner: recurse in user space if the type allows (depth
         * in the low 16 bits, clear of the ceiling), else report busy without
         * blocking, matching the non-ceiling trylock path. Otherwise take the
         * kernel path (ceiling check + boost) with an immediate timeout so a
         * contended trylock never blocks.
         */
        if ((mutex->owner & ~RTMUX_SYNC_WAITING) == id) {
            if ((mutex->count & RTMUX_SYNC_NONRECURSIVE) != 0)
                return EBUSY;
            if ((mutex->count & RTMUX_SYNC_PPDEPTHMASK) ==
                RTMUX_SYNC_PPDEPTHMASK)
                return EAGAIN;
            ++mutex->count;
            __ulock_debug_acquire(mutex, __pc);
            return EOK;
        }
        (void) __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_MUTEX, NULL,
                                 NULL, NULL);
        ret = __rtmux_pthread_ret(sync_mutex_lock_r((sync_t *) mutex));
        if (ret != EOK)
            return (ret == ETIMEDOUT) ? EBUSY : ret;
        ++mutex->count;
        __ulock_debug_acquire(mutex, __pc);
        return EOK;
    }

    /* Is it unlocked? */
    if ((owner = __mutex_smp_cmpxchg(&mutex->owner, 0, id)) == 0) {
        ++mutex->count;
        __ulock_debug_acquire(mutex, __pc);
        return EOK;
    }
    /* Is it recursive and locked by me? */
    if ((mutex->count & RTMUX_SYNC_NONRECURSIVE) == 0) {
        if ((owner & ~RTMUX_SYNC_WAITING) == id) {
            ++mutex->count;
            __ulock_debug_acquire(mutex, __pc);
            return EOK;
        }
    }
    /* Static intialized. Do an immediate tineout so no threads block. */
    if (owner == RTMUX_SYNC_INITIALIZER) {
        (void) __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_MUTEX, NULL,
                                 NULL, NULL);
        ret = __rtmux_pthread_ret(sync_mutex_lock_r((sync_t *) mutex));
        if (ret != EOK) {
            return (ret == ETIMEDOUT) ? EBUSY : ret;
        }

        ++mutex->count;
        __ulock_debug_acquire(mutex, __pc);
        return EOK;
    }

    if (mutex->owner == RTMUX_SYNC_DESTROYED) {
        return EINVAL;
    }

    return EBUSY;
}
