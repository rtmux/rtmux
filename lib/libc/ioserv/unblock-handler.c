/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <atomic.h>
#include <unistd.h>
#include <sys/ioserv-private.h>
#include "internal.h"

int _ioserv_unblock_handler(ioserv_context_t *ctp, ioserv_msg_t *msg, int rcvid)
{
    struct binding *binding;
    int (*func)(ioserv_context_t *ctp, ioserv_msg_t *msg, void *ocb);
    int ret = IOSERV_DEFAULT;
    struct _msg_info *infop = &ctp->info;

    if (msg_info(ctp->rcvid = rcvid, &ctp->info) == -1) {
        return IOSERV_DEFAULT;
    }

    if ((binding = (struct binding *) _ioserv_handle(
             &ctp->info, 0, _IOSERV_HANDLE_FIND_LOCK)) == (void *) -1) {
        SETIOV(ctp->iov + 0, msg, offsetof(struct _io_openfd, reserved2));
        if (msg_readv(ctp->rcvid, ctp->iov + 0, 1, 0) == -1) {
            return IOSERV_DEFAULT;
        }

        if (msg->type != _IO_OPENFD ||
            (binding = (struct binding *) _ioserv_handle(
                 &msg->openfd.i.info, 0, _IOSERV_HANDLE_FIND_LOCK)) ==
                (void *) -1) {
            if (msg->type != _IO_CONNECT) {
                return IOSERV_DEFAULT;
            }
            msg->connect.subtype = _IO_CONNECT_RSVD_UNBLOCK;
            return _ioserv_connect_handler(ctp, msg);
        }
        infop = &msg->openfd.i.info;
    }

    atomic_add(&binding->count, 1);
    _ioserv_handle(infop, 0, _IOSERV_HANDLE_UNLOCK);

    if ((func = _ioserv_io_func(binding->funcs, _IO_RSVD_UNBLOCK))) {
        ret = func(ctp, msg, binding->ocb);
    }

    _ioserv_close_handler(ctp, binding);

    return ret;
}
