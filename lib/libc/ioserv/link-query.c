/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic.h>
#include <pthread.h>
#include <sys/ioserv-private.h>
#include "internal.h"
#include "sleepon.h"


struct link *_ioserv_link_query(int id, int countadj)
{
    register struct link *p;

    __mutex_lock(&_ioserv_io_table.mutex);
    for (p = _ioserv_link_list; p; p = p->next) {
        if (p->id == id && !(p->flags & _IOSERV_LINK_DETACHWAIT)) {
            atomic_add(&p->count, (unsigned) countadj);
            __mutex_unlock(&_ioserv_io_table.mutex);
            return p;
        } else if (p->id >= id) {
            break;
        }
    }
    __mutex_unlock(&_ioserv_io_table.mutex);
    return 0;
}

void _ioserv_link_return(struct link *link, int countadj)
{
    atomic_sub(&link->count, (unsigned) countadj);

    /* Any time that this flag is raised, then once the count drops below one we
     * start signalling. There can only be one person waiting at a time in any
     * case, but they might have come in from a link handler.
     */
    if ((link->flags & _IOSERV_LINK_DETACHWAIT) && (link->count <= 1)) {
        _sleepon_lock(&_sleepon_default);
        (void) _sleepon_broadcast(&_sleepon_default, link);
        _sleepon_unlock(&_sleepon_default);
    }
}
