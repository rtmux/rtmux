/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include "cpucfg.h"

/* Mark the state a robust mutex protects as consistent again, after a lock
 * returned EOWNERDEAD. The caller owns the mutex, and no other thread or the
 * kernel writes count while it does, so a plain store suffices. Without this
 * call the next unlock leaves the mutex permanently ENOTRECOVERABLE.
 */
int pthread_mutex_consistent(pthread_mutex_t *mutex)
{
    if (!mutex || (mutex->count & RTMUX_SYNC_ROBUST) == 0 ||
        (mutex->count & RTMUX_SYNC_INCONSISTENT) == 0 ||
        (mutex->owner & ~RTMUX_SYNC_WAITING) != LIBC_TLS()->__owner)
        return EINVAL;

    mutex->count &= ~(long) RTMUX_SYNC_INCONSISTENT;
    return EOK;
}
