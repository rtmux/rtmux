/*
 * RTMux libc — POSIX thread barriers (_POSIX_BARRIERS).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A barrier is a process-private mutex + condition variable plus a generation
 * counter. Threads arriving at the barrier increment a waiter count under the
 * lock; the arrival that reaches the configured count bumps the generation,
 * resets the count, broadcasts, and returns PTHREAD_BARRIER_SERIAL_THREAD. The
 * others wait for the generation to change, which makes the wait immune to
 * spurious wakeups and to a fast thread re-entering the next round. Because the
 * mutex/cond carry per-process kernel state, PTHREAD_PROCESS_SHARED is not
 * offered.
 */

#include <errno.h>
#include <pthread.h>
#include <string.h>

int pthread_barrierattr_init(pthread_barrierattr_t *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof *attr);
    return EOK;
}

int pthread_barrierattr_destroy(pthread_barrierattr_t *attr)
{
    if (!attr)
        return EINVAL;
    return EOK;
}

int pthread_barrierattr_getpshared(const pthread_barrierattr_t *attr,
                                   int *pshared)
{
    if (!attr || !pshared)
        return EINVAL;
    *pshared = attr->flags & PTHREAD_PROCESSSHARED_MASK;
    return EOK;
}

int pthread_barrierattr_setpshared(pthread_barrierattr_t *attr, int pshared)
{
    if (!attr)
        return EINVAL;
    switch (pshared) {
    case PTHREAD_PROCESS_PRIVATE:
        attr->flags &= ~(unsigned) PTHREAD_PROCESSSHARED_MASK;
        return EOK;
    case PTHREAD_PROCESS_SHARED:
        return ENOTSUP;
    default:
        return EINVAL;
    }
}

int pthread_barrier_init(pthread_barrier_t *barrier,
                         const pthread_barrierattr_t *attr,
                         unsigned count)
{
    int ret;

    /* The only barrier attribute is pshared, and PTHREAD_PROCESS_SHARED is
     * already refused at pthread_barrierattr_setpshared() time, so a barrier is
     * always process-private and attr carries nothing else to consume.
     */
    (void) attr;
    if (!barrier || count == 0)
        return EINVAL;

    if ((ret = pthread_mutex_init(&barrier->lock, NULL)) != EOK)
        return ret;
    if ((ret = pthread_cond_init(&barrier->cond, NULL)) != EOK) {
        pthread_mutex_destroy(&barrier->lock);
        return ret;
    }
    barrier->count = count;
    barrier->waiting = 0;
    barrier->generation = 0;
    return EOK;
}

int pthread_barrier_destroy(pthread_barrier_t *barrier)
{
    int ret, cret;

    if (!barrier)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&barrier->lock)) != EOK)
        return ret;
    if (barrier->waiting != 0) {
        pthread_mutex_unlock(&barrier->lock);
        return EBUSY;
    }
    pthread_mutex_unlock(&barrier->lock);

    cret = pthread_cond_destroy(&barrier->cond);
    ret = pthread_mutex_destroy(&barrier->lock);
    return (ret != EOK) ? ret : cret;
}

int pthread_barrier_wait(pthread_barrier_t *barrier)
{
    unsigned long gen;
    int ret;

    if (!barrier)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&barrier->lock)) != EOK)
        return ret;

    if (++barrier->waiting == barrier->count) {
        barrier->generation++;
        barrier->waiting = 0;
        pthread_cond_broadcast(&barrier->cond);
        pthread_mutex_unlock(&barrier->lock);
        return PTHREAD_BARRIER_SERIAL_THREAD;
    }

    gen = barrier->generation;
    do {
        ret = pthread_cond_wait(&barrier->cond, &barrier->lock);
    } while (ret == EOK && gen == barrier->generation);
    pthread_mutex_unlock(&barrier->lock);
    return ret;
}
