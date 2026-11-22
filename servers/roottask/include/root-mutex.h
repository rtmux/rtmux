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
#endif
