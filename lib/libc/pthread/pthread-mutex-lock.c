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

    /* PRIO_PROTECT and robust mutexes need kernel participation even when
     * uncontended: for PRIO_PROTECT the kernel validates the caller priority
     * against the ceiling and applies the temporary ceiling boost; for a robust
     * mutex it records the holder, so that it can hand the mutex on with
     * EOWNERDEAD if the holder dies. Keep the lockless fast path for the
     * others.
     */
    if ((mutex->count & (RTMUX_SYNC_PRIOCEILING | RTMUX_SYNC_ROBUST)) != 0) {
        unsigned long depth_mask = rtmux_sync_depth_mask(mutex->count);

        /* A re-lock by the current owner of a RECURSIVE mutex recurses in user
         * space (the kernel already did its part on the first acquire); a
         * PRIO_PROTECT depth lives in the low 16 bits, clear of the ceiling.
         * Every other case (first/contended acquire, or a non-recursive re-lock
         * that must fail EDEADLK) goes through the kernel.
         */
        if ((mutex->owner & ~RTMUX_SYNC_WAITING) == id &&
            (mutex->count & RTMUX_SYNC_NONRECURSIVE) == 0) {
            if (((unsigned long) mutex->count & depth_mask) == depth_mask)
                return EAGAIN; /* Unix 98 recursive-count saturation */
            ++mutex->count;
            __ulock_debug_acquire(mutex, __pc);
            return EOK;
        }
        /* EOWNERDEAD comes with the mutex held, at depth 0. */
        ret = __rtmux_pthread_ret(sync_mutex_lock_r((sync_t *) mutex));
        if (ret != EOK && ret != EOWNERDEAD)
            return ret;
        ++mutex->count;
        __ulock_debug_acquire(mutex, __pc);
        return ret;
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
