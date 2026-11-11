/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>

static void pthread_once_cancel(void *data)
{
    pthread_mutex_t *mutex = data;

    pthread_mutex_unlock(mutex);
}

static int pthread_once_slow(pthread_once_t *once_control,
                             void (*init_routine)(void))
{
    int ret;

    if ((ret = pthread_mutex_lock(&once_control->mutex)) != EOK) {
        return (once_control->once != 0) ? EOK : ret;
    }

    if (once_control->once == 0) {
        pthread_cleanup_push(pthread_once_cancel, &once_control->mutex);
        init_routine();
        pthread_cleanup_pop(0);
        once_control->once = 1;
    }

    if (pthread_mutex_unlock(&once_control->mutex) == EOK) {
        pthread_mutex_destroy(&once_control->mutex);
    }
    return EOK;
}

int pthread_once(pthread_once_t *once_control, void (*init_routine)(void))
{
    if (once_control->once == 0) {
        return pthread_once_slow(once_control, init_routine);
    }
    return EOK;
}
