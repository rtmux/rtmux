/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include <stdlib.h>
#include <sys/ioserv-private.h>
#include "internal.h"

static pthread_once_t _ioserv_key_once = PTHREAD_ONCE_INIT;

static void key_once_func(void)
{
    (void) pthread_key_create(&_ioserv_thread_key, NULL);
}

struct link *_ioserv_link_alloc(void)
{
    register struct link *p, **pp;
    register int id;

    /* We do the key initialization here for a couple of reasons. First it is a
     * low bandwidth call, so we don't waste our time checking that it has been
     * called before. Second it is called by the name attach functions of both
     * the dispatch library and the historical I/O service code so we only do
     * this if the client is attaching a name.
     */
    pthread_once(&_ioserv_key_once, key_once_func);

    __mutex_lock(&_ioserv_io_table.mutex);
    for (id = 0, pp = &_ioserv_link_list; (p = *pp) && p->id == id;
         pp = &p->next, id++) {
        /* nothing to do */
    }
    if (!(p = calloc(sizeof *p, 1))) {
        __mutex_unlock(&_ioserv_io_table.mutex);
        return 0;
    }
    p->id = id;
    p->link_id = -1;
    p->flags = _IOSERV_LINK_HALFOPEN;
    p->next = *pp;
    *pp = p;
    __mutex_unlock(&_ioserv_io_table.mutex);
    return p;
}
