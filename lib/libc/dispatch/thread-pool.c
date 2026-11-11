/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/rtmux-core.h>
#include <string.h>
#include <stdlib.h>
#include <sys/dispatch.h>
#include "state.h"

static void thread_pool_attr_normalize(thread_pool_attr_t *tpattr)
{
    if (!tpattr)
        return;
    if (tpattr->hi_water > tpattr->maximum)
        tpattr->hi_water = tpattr->maximum;
    if (tpattr->lo_water > tpattr->hi_water)
        tpattr->lo_water = tpattr->hi_water;
}

thread_pool_t *thread_pool_create(thread_pool_attr_t *attr, unsigned flags)
{
    thread_pool_t *pool;
    struct _pool_properties *props;
    static const pthread_mutex_t _pool_mutex = PTHREAD_MUTEX_INITIALIZER;

    if (!(pool = malloc(sizeof *pool))) {
        return NULL;
    }
    memset(pool, 0, sizeof *pool);
    memcpy(pool, attr, sizeof *attr);
    pool->flags = flags;
    pool->props =
        (struct _pool_properties *) calloc(1, sizeof(struct _pool_properties));
    if (!pool->props) {
        free(pool);
        return NULL;
    }
    props = (struct _pool_properties *) pool->props;
    props->inline_lock = _pool_mutex;
    return pool;
}


int thread_pool_start(void *tp)
{
    thread_pool_t *pool = tp;
    struct _pool_properties *props;

    /* normalize values to be reasonably sane */
    thread_pool_attr_normalize(&pool->pool_attr);

    props = (struct _pool_properties *) pool->props;
    /* we do not need to lock here, since we are alone now */
    pool->created++;
    props->newthreads++;
    if (pool->flags & POOL_FLAG_RESERVE)
        props->reserved_threads++;
    if (pthread_create(0, pool->pool_attr.attr, _thread_pool_context_thread,
                       pool) != EOK) {
        /* we do not need to lock here, since we are alone now */
        pool->created--;
        props->newthreads--;
        return -1;
    }

    if (pool->flags & POOL_FLAG_EXIT_SELF) {
        pthread_detach(pthread_self());
        pthread_exit(NULL);
    }
    return EOK;
}
