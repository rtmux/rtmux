/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <errno.h>
#include <sys/rtmux-core.h>
#include "cpucfg.h"

int pthread_cond_init(pthread_cond_t *cond, const pthread_condattr_t *attr)
{
    return __rtmux_pthread_ret(
        sync_type_create_r(RTMUX_SYNC_COND, (sync_t *) cond, attr));
}
