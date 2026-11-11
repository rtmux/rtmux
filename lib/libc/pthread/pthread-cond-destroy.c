/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <sys/rtmux-core.h>
#include "cpucfg.h"

int pthread_cond_destroy(pthread_cond_t *cond)
{
    return __rtmux_pthread_ret(sync_destroy_r((sync_t *) cond));
}
