/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sys/rtmux-core.h>

/**
 * \brief Create a new thread
 */
int pthread_create(pthread_t *thread,
                   const pthread_attr_t *attr,
                   void *(*start_routine)(void *),
                   void *arg)
{
    int tid;

    while ((tid = thread_create_r(0, start_routine, arg,
                                  attr ? attr : &pthread_attr_default)) < 0) {
        if (tid != -EINTR) {
            return -tid;
        }
    }
    if (thread) {
        *thread = (pthread_t) tid;
    }
    return EOK;
}
