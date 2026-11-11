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

int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{
    /* A process-shared mutex is supported (the kernel keys sync objects by
     * physical address), but only with PTHREAD_PRIO_NONE: cross-process
     * priority inheritance/protection would have to boost owner threads across
     * process boundaries and is deferred, so reject the combination here.
     */
    if (attr && (attr->flags & PTHREAD_PROCESSSHARED_MASK) &&
        attr->protocol != PTHREAD_PRIO_NONE)
        return ENOTSUP;

    int ret = __rtmux_pthread_ret(
        sync_type_create_r(RTMUX_SYNC_MUTEX_FREE, (sync_t *) mutex, attr));
    if (ret != EOK)
        return ret;

    /* The kernel seeds count from the protocol only; translate the attr mutex
     * TYPE into the NONRECURSIVE/NOERRORCHECK count bits that the lock,
     * trylock, and unlock fast paths read. RECURSIVE clears NONRECURSIVE (a
     * re-lock by the owner bumps the count); ERRORCHECK and DEFAULT keep
     * NONRECURSIVE with error checking (a re-lock is EDEADLK); NORMAL adds
     * NOERRORCHECK (a re-lock deadlocks). PRIO_PROTECT mutexes go through this
     * too: their ceiling lives in disjoint high bits (RTMUX_SYNC_CEIL_MASK), so
     * setting the type bits leaves the ceiling intact and a PRIO_PROTECT +
     * RECURSIVE mutex recurses like any other recursive mutex.
     */
    long count = mutex->count &
                 ~(long) (RTMUX_SYNC_NONRECURSIVE | RTMUX_SYNC_NOERRORCHECK);
    if (!(attr && (attr->flags & PTHREAD_RECURSIVE_ENABLE)))
        count |= (long) RTMUX_SYNC_NONRECURSIVE;
    if (attr && (attr->flags & PTHREAD_ERRORCHECK_DISABLE))
        count |= (long) RTMUX_SYNC_NOERRORCHECK;
    mutex->count = count;
    return EOK;
}
