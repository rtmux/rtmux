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

/* pthread_mutex_lock is not a cancellation point and must not return EINTR. */

int(pthread_mutex_lock)(pthread_mutex_t *mutex)
{
    const void *__attribute__((__unused__)) __pc = __builtin_return_address(0);
    unsigned long id = LIBC_TLS()->__owner;
    int ret;

    /* PRIO_PROTECT needs kernel participation even when uncontended: the kernel
     * validates the caller priority against the ceiling and applies the
     * temporary ceiling boost. Keep the lockless fast path for PRIO_NONE and
     * PRIO_INHERIT only.
     */
    if ((mutex->count & RTMUX_SYNC_PRIOCEILING) != 0) {
        /* A re-lock by the current owner of a RECURSIVE ceiling mutex recurses
         * in user space (the kernel already checked the ceiling and applied the
         * boost on the first acquire); the depth lives in the low 16 bits,
         * clear of the ceiling. Every other case (first/contended acquire, or a
         * non-recursive re-lock that must fail EDEADLK) goes through the
         * kernel.
         */
        if ((mutex->owner & ~RTMUX_SYNC_WAITING) == id &&
            (mutex->count & RTMUX_SYNC_NONRECURSIVE) == 0) {
            if ((mutex->count & RTMUX_SYNC_PPDEPTHMASK) ==
                RTMUX_SYNC_PPDEPTHMASK)
                return EAGAIN; /* Unix 98 recursive-count saturation */
            ++mutex->count;
            __ulock_debug_acquire(mutex, __pc);
            return EOK;
        }
        if ((ret = sync_mutex_lock_r((sync_t *) mutex)) != EOK)
            return __rtmux_pthread_ret(ret);
        ++mutex->count;
        __ulock_debug_acquire(mutex, __pc);
        return EOK;
    }

    /* Fast path: CAS the owner field from unlocked to caller. */
    if (__mutex_smp_cmpxchg(&mutex->owner, 0, id) == 0) {
        ++mutex->count;
        __ulock_debug_acquire(mutex, __pc);
        return EOK;
    }
    /* Already owned by the caller: recursive bump, or EDEADLK / EAGAIN. */
    if ((mutex->owner & ~RTMUX_SYNC_WAITING) == id) {
        if ((mutex->count & RTMUX_SYNC_NONRECURSIVE) == 0) {
            if ((mutex->count & RTMUX_SYNC_COUNTMASK) == RTMUX_SYNC_COUNTMASK) {
                return EAGAIN; /* Unix 98 recursive-count saturation */
            }
            ++mutex->count;
            __ulock_debug_acquire(mutex, __pc);
            return EOK;
        }
        if ((mutex->count & RTMUX_SYNC_NOERRORCHECK) == 0) {
            return EDEADLK;
        }
    }
    /* Contended: drop into the kernel to wait. */
    if ((ret = sync_mutex_lock_r((sync_t *) mutex)) != EOK) {
        return __rtmux_pthread_ret(ret);
    }
    ++mutex->count;
    __ulock_debug_acquire(mutex, __pc);
    return EOK;
}
