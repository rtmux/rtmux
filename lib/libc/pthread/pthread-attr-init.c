/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/rtmux-core.h>

int pthread_attr_init(pthread_attr_t *attr)
{
    static long pagesize;
    int ret;

    if (!attr)
        return EINVAL;

    *attr = pthread_attr_default;
    if ((ret = __sched_get_r(0, 0, (struct sched_param *) &attr->sched_param)) <
        0) {
        return -ret;
    }
    attr->policy = ret;

    if (pagesize == 0) {
        ret = errno;
        if ((pagesize = sysconf(_SC_PAGESIZE)) == -1) {
            errno = ret;
        }
    }

    attr->guardsize = pagesize <= 0 ? 0 : (unsigned) pagesize;

    return EOK;
}
