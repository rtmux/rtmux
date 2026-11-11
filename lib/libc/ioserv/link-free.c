/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioserv-private.h>
#include "internal.h"

int _ioserv_link_free(int id, unsigned flags)
{
    register struct link *p, **pp;

    __mutex_lock(&_ioserv_io_table.mutex);
    for (pp = &_ioserv_link_list; (p = *pp); pp = &p->next) {
        if (p->id == id) {
            if (p->link_id != -1) {
                close(p->link_id);
                p->link_id = -1;
            }
            if (!(flags & IOSERV_DETACH_PATHNAME)) {
                *pp = p->next;
                free(p);
            }
            __mutex_unlock(&_ioserv_io_table.mutex);
            return 0;
        }
    }
    errno = EINVAL;
    __mutex_unlock(&_ioserv_io_table.mutex);
    return -1;
}
