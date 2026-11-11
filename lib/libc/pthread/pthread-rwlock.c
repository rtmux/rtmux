/*
 * RTMux libc — POSIX read-write locks (_POSIX_READER_WRITER_LOCKS).
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The lock is a process-private mutex guarding a small state machine plus two
 * condition variables — one for waiting readers, one for waiting writers.
 * `active` counts held read locks, is -1 while a writer holds the lock, and 0
 * when free. A writer is preferred only at the moment the lock is free: a new
 * reader yields to a waiting writer when `active == 0`, but joins immediately
 * when readers already hold the lock (`active > 0`). That join is deliberate —
 * without it a thread that already owns a read lock would block behind a queued
 * writer that is itself waiting for that thread's read lock to drop, a deadlock
 * POSIX explicitly warns about (a thread may hold multiple concurrent read
 * locks). Because the mutex/cond carry per-process kernel state,
 * PTHREAD_PROCESS_SHARED is not offered.
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>

int pthread_rwlockattr_init(pthread_rwlockattr_t *attr)
{
    if (!attr)
        return EINVAL;
    memset(attr, 0, sizeof *attr);
    return EOK;
}

int pthread_rwlockattr_destroy(pthread_rwlockattr_t *attr)
{
    if (!attr)
        return EINVAL;
    return EOK;
}

int pthread_rwlockattr_getpshared(const pthread_rwlockattr_t *attr,
                                  int *pshared)
{
    if (!attr || !pshared)
        return EINVAL;
    *pshared = attr->flags & PTHREAD_PROCESSSHARED_MASK;
    return EOK;
}

int pthread_rwlockattr_setpshared(pthread_rwlockattr_t *attr, int pshared)
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

int pthread_rwlock_init(pthread_rwlock_t *rwlock,
                        const pthread_rwlockattr_t *attr)
{
    int ret;

    /* pshared is the only attribute, and PTHREAD_PROCESS_SHARED is refused at
     * pthread_rwlockattr_setpshared() time, so attr carries nothing to use.
     */
    (void) attr;
    if (!rwlock)
        return EINVAL;

    if ((ret = pthread_mutex_init(&rwlock->lock, NULL)) != EOK)
        return ret;
    if ((ret = pthread_cond_init(&rwlock->readers, NULL)) != EOK) {
        pthread_mutex_destroy(&rwlock->lock);
        return ret;
    }
    if ((ret = pthread_cond_init(&rwlock->writers, NULL)) != EOK) {
        pthread_cond_destroy(&rwlock->readers);
        pthread_mutex_destroy(&rwlock->lock);
        return ret;
    }
    rwlock->active = 0;
    rwlock->waiting_writers = 0;
    return EOK;
}

int pthread_rwlock_destroy(pthread_rwlock_t *rwlock)
{
    int ret, rret, wret;

    if (!rwlock)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&rwlock->lock)) != EOK)
        return ret;
    if (rwlock->active != 0 || rwlock->waiting_writers != 0) {
        pthread_mutex_unlock(&rwlock->lock);
        return EBUSY;
    }
    pthread_mutex_unlock(&rwlock->lock);

    rret = pthread_cond_destroy(&rwlock->readers);
    wret = pthread_cond_destroy(&rwlock->writers);
    ret = pthread_mutex_destroy(&rwlock->lock);
    if (ret != EOK)
        return ret;
    return (rret != EOK) ? rret : wret;
}

/* The lock just became free (or a waiting writer is bowing out): hand it to a
 * waiting writer if one exists, otherwise reopen the gate for all readers.
 */
static void rwlock_wake_next(pthread_rwlock_t *rwlock)
{
    if (rwlock->waiting_writers != 0)
        pthread_cond_signal(&rwlock->writers);
    else
        pthread_cond_broadcast(&rwlock->readers);
}

/* A new reader must wait while a writer holds the lock (active < 0), or yield
 * to a queued writer only while the lock is free (active == 0). Once readers
 * already hold it (active > 0) a new reader joins them, so an already-held read
 * lock can nest without deadlocking behind a queued writer.
 */
static int reader_must_wait(const pthread_rwlock_t *rwlock)
{
    return rwlock->active < 0 ||
           (rwlock->active == 0 && rwlock->waiting_writers != 0);
}

/* Shared read/write acquisition. A NULL abstime blocks indefinitely; a non-NULL
 * one bounds the wait and surfaces ETIMEDOUT.
 */

static int rwlock_rdlock(pthread_rwlock_t *rwlock,
                         const struct timespec *abstime)
{
    int ret;

    if (!rwlock)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&rwlock->lock)) != EOK)
        return ret;

    while (reader_must_wait(rwlock)) {
        ret = abstime ? pthread_cond_timedwait(&rwlock->readers, &rwlock->lock,
                                               abstime)
                      : pthread_cond_wait(&rwlock->readers, &rwlock->lock);
        if (ret != EOK) {
            pthread_mutex_unlock(&rwlock->lock);
            return ret;
        }
    }
    if (rwlock->active == INT_MAX) { /* POSIX: max read locks exceeded */
        pthread_mutex_unlock(&rwlock->lock);
        return EAGAIN;
    }
    rwlock->active++;
    pthread_mutex_unlock(&rwlock->lock);
    return EOK;
}

static int rwlock_wrlock(pthread_rwlock_t *rwlock,
                         const struct timespec *abstime)
{
    int ret;

    if (!rwlock)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&rwlock->lock)) != EOK)
        return ret;

    rwlock->waiting_writers++;
    while (rwlock->active != 0) {
        ret = abstime ? pthread_cond_timedwait(&rwlock->writers, &rwlock->lock,
                                               abstime)
                      : pthread_cond_wait(&rwlock->writers, &rwlock->lock);
        if (ret != EOK) {
            rwlock->waiting_writers--;

            /* A prior unlock may have signalled *this* writer to take a
             * now-free lock; since we are leaving, pass that wakeup on. A
             * needless signal is harmless — the woken thread just rechecks
             * state and sleeps.
             */
            rwlock_wake_next(rwlock);
            pthread_mutex_unlock(&rwlock->lock);
            return ret;
        }
    }
    rwlock->waiting_writers--;
    rwlock->active = -1;
    pthread_mutex_unlock(&rwlock->lock);
    return EOK;
}

int pthread_rwlock_rdlock(pthread_rwlock_t *rwlock)
{
    return rwlock_rdlock(rwlock, NULL);
}

int pthread_rwlock_timedrdlock(pthread_rwlock_t *rwlock,
                               const struct timespec *abstime)
{
    if (!abstime)
        return EINVAL;
    return rwlock_rdlock(rwlock, abstime);
}

int pthread_rwlock_wrlock(pthread_rwlock_t *rwlock)
{
    return rwlock_wrlock(rwlock, NULL);
}

int pthread_rwlock_timedwrlock(pthread_rwlock_t *rwlock,
                               const struct timespec *abstime)
{
    if (!abstime)
        return EINVAL;
    return rwlock_wrlock(rwlock, abstime);
}

int pthread_rwlock_tryrdlock(pthread_rwlock_t *rwlock)
{
    int ret;

    if (!rwlock)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&rwlock->lock)) != EOK)
        return ret;
    if (reader_must_wait(rwlock)) {
        pthread_mutex_unlock(&rwlock->lock);
        return EBUSY;
    }
    if (rwlock->active == INT_MAX) { /* POSIX: max read locks exceeded */
        pthread_mutex_unlock(&rwlock->lock);
        return EAGAIN;
    }
    rwlock->active++;
    pthread_mutex_unlock(&rwlock->lock);
    return EOK;
}

int pthread_rwlock_trywrlock(pthread_rwlock_t *rwlock)
{
    int ret;

    if (!rwlock)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&rwlock->lock)) != EOK)
        return ret;
    if (rwlock->active != 0) {
        pthread_mutex_unlock(&rwlock->lock);
        return EBUSY;
    }
    rwlock->active = -1;
    pthread_mutex_unlock(&rwlock->lock);
    return EOK;
}

int pthread_rwlock_unlock(pthread_rwlock_t *rwlock)
{
    int ret;

    if (!rwlock)
        return EINVAL;
    if ((ret = pthread_mutex_lock(&rwlock->lock)) != EOK)
        return ret;

    if (rwlock->active < 0)
        rwlock->active = 0; /* a writer released */
    else if (rwlock->active > 0)
        rwlock->active--; /* a reader released */
    else {
        pthread_mutex_unlock(&rwlock->lock);
        return EPERM; /* not locked */
    }

    /* Once the lock is free, prefer a waiting writer over readers. */
    if (rwlock->active == 0)
        rwlock_wake_next(rwlock);
    pthread_mutex_unlock(&rwlock->lock);
    return EOK;
}
