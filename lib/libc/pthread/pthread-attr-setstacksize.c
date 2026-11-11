/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sys/rtmux-core.h>

int pthread_attr_setstacksize(pthread_attr_t *attr, size_t stacksize)
{
    if (!attr || (stacksize != 0 && stacksize < PTHREAD_STACK_MIN))
        return EINVAL;

    attr->stacksize = stacksize;
    return EOK;
}
