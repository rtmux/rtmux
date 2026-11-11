/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <sys/rtmux-core.h>
#include "cpucfg.h"

int pthread_detach(pthread_t thread)
{
    return __rtmux_pthread_ret(thread_detach_r(thread));
}
