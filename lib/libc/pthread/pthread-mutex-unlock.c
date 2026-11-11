/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sys/rtmux-core.h>
#include <cpu-inline.h>
#define mem_barrier() __cpu_membarrier()
#include "cpucfg.h"
#include "pthread-ulock-debug.h"

int(pthread_mutex_unlock)(pthread_mutex_t *mutex)
{
    if ((mutex->owner & ~RTMUX_SYNC_WAITING) == LIBC_TLS()->__owner) {
        unsigned long flags = (unsigned long) mutex->count &
                              ~(unsigned long) RTMUX_SYNC_COUNTMASK;
        unsigned long depth =
            (unsigned long) mutex->count & RTMUX_SYNC_COUNTMASK;

        if ((mutex->count & RTMUX_SYNC_PRIOCEILING) != 0) {
            /* The ceiling sits in disjoint high bits, so decrement only the
             * low-16 depth. A recursive unlock (depth > 1) stays held entirely
             * in user space; the final unlock drops to depth 0 and enters the
             * kernel to lift the ceiling boost and release/hand off the owner
             * (owner stays == self until that kcall, so no self-serve race).
             */
            unsigned long d =
                (unsigned long) mutex->count & RTMUX_SYNC_PPDEPTHMASK;
            if (d > 1) {
                --mutex->count;
                return EOK;
            }
            if (d == 1)
                --mutex->count;

            /* Final unlock of the ceiling mutex: the lock is really released.
             */
            __ulock_debug_release(mutex);
            return __rtmux_pthread_ret(sync_mutex_unlock_r((sync_t *) mutex));
        }
        if (depth != 0)
            depth--;
        mutex->count = (long) (flags | depth);
        if (depth == 0) {
            /* Recursive count hit zero: the mutex is really being released. */
            __ulock_debug_release(mutex);
            mem_barrier();
            if (__mutex_release_owner(&mutex->owner) & RTMUX_SYNC_WAITING) {
                return __rtmux_pthread_ret(
                    sync_mutex_unlock_r((sync_t *) mutex));
            }
        }
        return EOK;
    }
    return EPERM;
}
