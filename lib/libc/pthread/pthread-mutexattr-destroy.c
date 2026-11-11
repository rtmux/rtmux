/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>

int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{
    if (!attr)
        return EINVAL;

    return EOK;
}
