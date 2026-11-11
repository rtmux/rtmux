/*
 * pthread_condattr process-shared attribute.
 *
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 RTMux contributors
 */

#include <errno.h>
#include <pthread.h>

int pthread_condattr_getpshared(const pthread_condattr_t *attr, int *pshared)
{
    if (!attr || !pshared)
        return EINVAL;

    *pshared = attr->flags & PTHREAD_PROCESSSHARED_MASK;
    return EOK;
}

int pthread_condattr_setpshared(pthread_condattr_t *attr, int pshared)
{
    if (!attr)
        return EINVAL;

    switch (pshared) {
    case PTHREAD_PROCESS_PRIVATE:
        attr->flags &= ~(unsigned) PTHREAD_PROCESSSHARED_MASK;
        return EOK;
    case PTHREAD_PROCESS_SHARED:
        /* Physical-address keying in the kernel sync layer makes a condvar in
         * shared memory work across processes.
         */
        attr->flags |= PTHREAD_PROCESSSHARED_MASK;
        return EOK;
    default:
        return EINVAL;
    }
}
