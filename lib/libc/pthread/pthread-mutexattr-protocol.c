/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>

static int valid_protocol(int protocol)
{
    return protocol == PTHREAD_PRIO_INHERIT || protocol == PTHREAD_PRIO_NONE ||
           protocol == PTHREAD_PRIO_PROTECT;
}

static int valid_type(int type)
{
    return type == PTHREAD_MUTEX_DEFAULT || type == PTHREAD_MUTEX_NORMAL ||
           type == PTHREAD_MUTEX_ERRORCHECK || type == PTHREAD_MUTEX_RECURSIVE;
}

int pthread_mutexattr_getprotocol(const pthread_mutexattr_t *attr,
                                  int *protocol)
{
    if (!attr || !protocol)
        return EINVAL;
    *protocol = attr->protocol;
    return EOK;
}

int pthread_mutexattr_setprotocol(pthread_mutexattr_t *attr, int protocol)
{
    if (!attr || !valid_protocol(protocol))
        return EINVAL;
    attr->protocol = protocol;
    return EOK;
}

int pthread_mutexattr_getprioceiling(const pthread_mutexattr_t *attr,
                                     int *prioceiling)
{
    if (!attr || !prioceiling)
        return EINVAL;
    *prioceiling = attr->prioceiling;
    return EOK;
}

int pthread_mutexattr_setprioceiling(pthread_mutexattr_t *attr, int prioceiling)
{
    /* The ceiling occupies its own 8-bit field (RTMUX_SYNC_CEIL_MASK), disjoint
     * from the recursion depth, so it must be a valid 1-255 priority.
     */
    if (!attr || prioceiling <= 0 ||
        (unsigned long) prioceiling >
            (RTMUX_SYNC_CEIL_MASK >> RTMUX_SYNC_CEIL_SHIFT))
        return EINVAL;
    attr->prioceiling = prioceiling;
    return EOK;
}

int pthread_mutexattr_getpshared(const pthread_mutexattr_t *attr, int *pshared)
{
    if (!attr || !pshared)
        return EINVAL;

    *pshared = attr->flags & PTHREAD_PROCESSSHARED_MASK;
    return EOK;
}

int pthread_mutexattr_setpshared(pthread_mutexattr_t *attr, int pshared)
{
    if (!attr)
        return EINVAL;

    switch (pshared) {
    case PTHREAD_PROCESS_PRIVATE:
        attr->flags &= ~(unsigned) PTHREAD_PROCESSSHARED_MASK;
        return EOK;
    case PTHREAD_PROCESS_SHARED:
        /* Backed by physical-address keying in the kernel sync layer, so a
         * mutex in shared memory rendezvouses across processes. A shared mutex
         * must stay PTHREAD_PRIO_NONE, though: cross-process priority
         * inheritance/protection is unsupported and pthread_mutex_init rejects
         * the combination.
         */
        attr->flags |= PTHREAD_PROCESSSHARED_MASK;
        return EOK;
    default:
        return EINVAL;
    }
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{
    if (!attr || !type)
        return EINVAL;

    if ((attr->flags & PTHREAD_MUTEX_TYPE) == 0) {
        *type = PTHREAD_MUTEX_DEFAULT;
    } else if (attr->flags & PTHREAD_RECURSIVE_ENABLE) {
        *type = PTHREAD_MUTEX_RECURSIVE;
    } else if (attr->flags & PTHREAD_ERRORCHECK_DISABLE) {
        *type = PTHREAD_MUTEX_NORMAL;
    } else {
        *type = PTHREAD_MUTEX_ERRORCHECK;
    }
    return EOK;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{
    if (!attr || !valid_type(type))
        return EINVAL;

    attr->flags &= ~(unsigned) (PTHREAD_RECURSIVE_MASK |
                                PTHREAD_ERRORCHECK_MASK | PTHREAD_MUTEX_TYPE);
    switch (type) {
    case PTHREAD_MUTEX_DEFAULT:
        break;
    case PTHREAD_MUTEX_RECURSIVE:
        attr->flags |= PTHREAD_MUTEX_TYPE | PTHREAD_RECURSIVE_ENABLE;
        break;
    case PTHREAD_MUTEX_NORMAL:
        attr->flags |= PTHREAD_MUTEX_TYPE | PTHREAD_ERRORCHECK_DISABLE;
        break;
    case PTHREAD_MUTEX_ERRORCHECK:
        attr->flags |= PTHREAD_MUTEX_TYPE;
        break;
    }
    return EOK;
}

int pthread_mutex_getprioceiling(const pthread_mutex_t *mutex, int *prioceiling)
{
    unsigned long count;

    if (!mutex || !prioceiling)
        return EINVAL;
    count = (unsigned long) mutex->count;
    if ((count & RTMUX_SYNC_PRIOCEILING) == 0)
        return EINVAL;

    /* The ceiling sits in its own bits, independent of the recursion depth, so
     * it reads back directly whether the mutex is free or held.
     */
    *prioceiling =
        (int) ((count & RTMUX_SYNC_CEIL_MASK) >> RTMUX_SYNC_CEIL_SHIFT);
    return EOK;
}

int pthread_mutex_setprioceiling(pthread_mutex_t *mutex,
                                 int prioceiling,
                                 int *old_ceiling)
{
    const unsigned long ceil_max =
        RTMUX_SYNC_CEIL_MASK >> RTMUX_SYNC_CEIL_SHIFT;
    unsigned long old_count;
    int rc;

    if (!mutex || prioceiling <= 0 || (unsigned long) prioceiling > ceil_max)
        return EINVAL;
    if (((unsigned long) mutex->count & RTMUX_SYNC_PRIOCEILING) == 0)
        return EINVAL;

    /* POSIX: lock the mutex (blocking), change the ceiling, then unlock.
     * Holding it guarantees no other thread is running below the old ceiling
     * while we mutate it. The ceiling has its own bits, so rewrite just those
     * and leave the recursion depth and protocol/type bits untouched.
     *
     * If the caller ALREADY holds this mutex (only possible for a RECURSIVE
     * ceiling mutex), the boost applied on the original acquire was for the old
     * ceiling and is not re-derived here, so the caller stays boosted at the
     * old ceiling until it fully unwinds. The kernel's held-ceiling tracking
     * then hits its conservative "not found" path on the final unlock, which
     * never under-protects — an accepted latency edge for this exotic pattern.
     */
    rc = pthread_mutex_lock(mutex);
    if (rc != EOK)
        return rc;
    old_count = (unsigned long) mutex->count;
    if (old_ceiling)
        *old_ceiling =
            (int) ((old_count & RTMUX_SYNC_CEIL_MASK) >> RTMUX_SYNC_CEIL_SHIFT);
    mutex->count =
        (long) ((old_count & ~(unsigned long) RTMUX_SYNC_CEIL_MASK) |
                ((unsigned long) prioceiling << RTMUX_SYNC_CEIL_SHIFT));
    return pthread_mutex_unlock(mutex);
}
