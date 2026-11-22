/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_ROOT_MUTEX_H
#define RTMUX_ROOT_MUTEX_H

#include <errno.h>
#include <pthread.h>
#include <time.h>

/* A PI mutex can run out of turnstiles. Sleep so a lower-priority owner
 * can release one. Callers may only proceed with ownership. */
static inline void root_mutex_lock(pthread_mutex_t *mutex)
{
    int err;
    while ((err = pthread_mutex_lock(mutex)) == EAGAIN) {
        struct timespec delay = {.tv_nsec = 1000000};
        nanosleep(&delay, NULL);
    }
    if (err)
        __builtin_trap();
}

/* A bounded wait for something its holders give back soon, under a mutex the
 * caller holds for every use: at most `max` threads wait at once, and each
 * until a deadline on CLOCK_MONOTONIC, which no one can set back. A waiter
 * takes a pool thread, so `max` keeps a share of the pool free for the work
 * that ends the wait.
 */
struct root_wait {
    pthread_cond_t cond; /* set up by the first root_wait() */
    unsigned waiting;    /* threads in root_wait() */
    unsigned max;        /* the most that may be */
    int ready;           /* cond is set up */
};
#define ROOT_WAIT_INITIALIZER(n) {.max = (n)}

/* Wait for root_wake(). 0 when woken, after which the caller tests its
 * condition again; EAGAIN when `max` threads wait already, the deadline has
 * passed or the wait failed. *deadline starts zeroed and is set by the first
 * wait to `s` seconds from then, so that a caller that never waits reads no
 * clock; later waits of the same caller keep it.
 */
static inline int root_wait(struct root_wait *w, pthread_mutex_t *mutex,
                            struct timespec *deadline, time_t s)
{
    int r;

    if (w->waiting == w->max)
        return EAGAIN;
    if (!deadline->tv_sec && !deadline->tv_nsec) {
        (void) clock_gettime(CLOCK_MONOTONIC, deadline);
        deadline->tv_sec += s;
    }
    if (!w->ready) {
        pthread_condattr_t attr;

        if (pthread_condattr_init(&attr) ||
            pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) ||
            pthread_cond_init(&w->cond, &attr))
            __builtin_trap();
        (void) pthread_condattr_destroy(&attr);
        w->ready = 1;
    }
    w->waiting++;
    r = pthread_cond_timedwait(&w->cond, mutex, deadline);
    w->waiting--;
    return r ? EAGAIN : 0;
}

/* Wake every waiter, each to test its own condition. */
static inline void root_wake(struct root_wait *w)
{
    if (w->waiting)
        (void) pthread_cond_broadcast(&w->cond);
}
#endif
