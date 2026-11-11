/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>

extern void __my_thread_exit(void *) __hidden;

const pthread_attr_t pthread_attr_default __hidden = {
    .flags = PTHREAD_CREATE_JOINABLE | PTHREAD_SCOPE_SYSTEM |
             PTHREAD_CANCEL_DEFERRED | PTHREAD_CANCEL_ENABLE |
             PTHREAD_INHERIT_SCHED,
    .stacksize = 0,
    .stackaddr = NULL,
    .exitfunc = __my_thread_exit,
    .policy = -1,
    .sched_param = {.sched_priority = -1, .sched_curpriority = -1},
    .guardsize = __PAGESIZE,
    .prealloc = 0,
};
