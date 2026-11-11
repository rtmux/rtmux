/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * pthread_attr_setdetachstate() — set the detach state attribute.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>

int pthread_attr_setdetachstate(pthread_attr_t *attr, int detachstate)
{
    if (!attr)
        return EINVAL;

    if (detachstate != PTHREAD_CREATE_JOINABLE &&
        detachstate != PTHREAD_CREATE_DETACHED)
        return EINVAL;

    if (detachstate == PTHREAD_CREATE_DETACHED)
        attr->flags |= PTHREAD_CREATE_DETACHED;
    else
        attr->flags &= ~PTHREAD_CREATE_DETACHED;
    return EOK;
}
