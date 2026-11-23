/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_ROOT_MUTEX_H
#define RTMUX_ROOT_MUTEX_H

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

/* A PI mutex can run out of turnstiles. Sleep so a lower-priority owner can
 * release one. Callers may only proceed with ownership.
 */
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
 * that ends the wait. A place in the arrival order of a root_wait
 * (root_ticket_join): it lives on its waiter's stack, and `key` names what the
 * waiter waits for, so that waiters for different things do not queue behind
 * each other.
 */
struct root_ticket {
    struct root_ticket *next;
    unsigned key;
};

struct root_wait {
    pthread_cond_t cond;       /* set up by the first root_wait_until() */
    unsigned waiting;          /* threads in root_wait_until() */
    unsigned max;              /* the most that may be */
    int ready;                 /* cond is set up */
    struct root_ticket *queue; /* tickets, oldest first */
    struct root_ticket *
        *tail;        /* the last next pointer; NULL until the first join */
    unsigned tickets; /* tickets queued, waiting or not */
};
#define ROOT_WAIT_INITIALIZER(n) {.max = (n)}

/* Wait for root_wake() until *deadline (CLOCK_MONOTONIC): 0 when woken,
 * ETIMEDOUT at the deadline, EAGAIN when `max` threads wait already or the wait
 * failed.
 */
static inline int root_wait_until(struct root_wait *w,
                                  pthread_mutex_t *mutex,
                                  const struct timespec *deadline)
{
    int r;

    if (w->waiting == w->max)
        return EAGAIN;
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
    return !r ? 0 : r == ETIMEDOUT ? ETIMEDOUT : EAGAIN;
}

/* CLOCK_MONOTONIC now, in ns. */
static inline uint64_t root_now_ns(void)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return timespec2nsec(&ts);
}

/* A wait's deadline in ns of CLOCK_MONOTONIC, *until, kept by its caller across
 * waits: set to s seconds from now when still 0. EAGAIN once it has passed,
 * else 0.
 */
static inline int root_deadline_ns(uint64_t *until, unsigned s)
{
    uint64_t now = root_now_ns();

    if (!*until)
        *until = now + (uint64_t) s * 1000000000u;
    return now >= *until ? EAGAIN : 0;
}

/* root_wait_until with the deadline in ns of CLOCK_MONOTONIC. */
static inline int root_wait_until_ns(struct root_wait *w,
                                     pthread_mutex_t *mutex,
                                     uint64_t ns)
{
    struct timespec ts = {.tv_sec = (time_t) (ns / 1000000000u),
                          .tv_nsec = (long) (ns % 1000000000u)};

    return root_wait_until(w, mutex, &ts);
}

/* Wake every waiter, each to test its own condition. */
static inline void root_wake(struct root_wait *w)
{
    if (w->waiting)
        (void) pthread_cond_broadcast(&w->cond);
}

/* Arrival order. A waiter that must not be overtaken joins the queue before its
 * first wait, takes what it waits for only once root_ticket_first() says no
 * older ticket of its key is queued, and leaves the queue on every way out. A
 * newcomer that finds root_ticket_queued() for its key joins behind them
 * instead of taking. The queue holds one ticket per waiter, so its walks are
 * bounded by `max` (plus the caller). All under the mutex of the wait.
 */
static inline bool root_ticket_queued(const struct root_wait *w, unsigned key)
{
    for (const struct root_ticket *t = w->queue; t; t = t->next)
        if (t->key == key)
            return true;
    return false;
}

static inline void root_ticket_join(struct root_wait *w,
                                    struct root_ticket *t,
                                    unsigned key)
{
    if (!w->tail)
        w->tail = &w->queue;
    t->next = NULL;
    t->key = key;
    *w->tail = t;
    w->tail = &t->next;
    w->tickets++;
}

static inline bool root_ticket_first(const struct root_wait *w,
                                     const struct root_ticket *t)
{
    for (const struct root_ticket *u = w->queue; u != t; u = u->next)
        if (u->key == t->key)
            return false;
    return true;
}

/* Leave the queue. A ticket that was first of its key hands the turn on: the
 * waiters are woken to look again.
 */
static inline void root_ticket_leave(struct root_wait *w, struct root_ticket *t)
{
    struct root_ticket **pp = &w->queue;
    bool first = true, next = false;

    for (; *pp != t; pp = &(*pp)->next)
        if ((*pp)->key == t->key)
            first = false;
    *pp = t->next;
    if (w->tail == &t->next)
        w->tail = pp;
    w->tickets--;
    for (const struct root_ticket *u = t->next; u && first && !next;
         u = u->next)
        next = u->key == t->key;
    if (next)
        root_wake(w);
}
#endif
