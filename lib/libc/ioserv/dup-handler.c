/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <sys/rtmux-core.h>
#include <atomic.h>
#include <sys/ioserv-private.h>
#include "internal.h"

int _ioserv_dup_handler(ioserv_context_t *ctp, ioserv_msg_t *msg)
{
    int n, id;
    struct binding *binding;
    _ioserv_func_t func;

    if ((binding = (struct binding *) _ioserv_handle(
             &msg->dup.i.info, 0, _IOSERV_HANDLE_FIND_LOCK)) == (void *) -1) {
        return EBADF;
    }

    atomic_add(&binding->count, 1);
    _ioserv_handle(&msg->dup.i.info, 0, _IOSERV_HANDLE_UNLOCK);

    n = _ioserv_access(ctp, &msg->dup.i.info, msg->dup.i.key, msg,
                       offsetof(struct _io_dup, key));
    if (n != EOK) {
        _ioserv_close_handler(ctp, binding);
        return n;
    }

    ctp->id = binding->id;
    if ((func = _ioserv_io_func(binding->funcs, _IO_DUP))) {
        switch ((unsigned) (n = func(ctp, msg, binding->ocb))) {
        case (unsigned) IOSERV_DEFAULT:
        case EOK:
            break;
        default:
            _ioserv_close_handler(ctp, binding);
            return n;
        }
    }


    if (_ioserv_handle(&ctp->info, binding, _IOSERV_HANDLE_SET) !=
        (void *) -1) {
        return n;
    }
    n = errno;

    if (func && (func = _ioserv_io_func(binding->funcs, _IO_CLOSE))) {
        id = ctp->rcvid;
        ctp->rcvid = 0;
        (void) func(ctp, msg, binding->ocb);
        ctp->rcvid = id;
    }

    _ioserv_close_handler(ctp, binding);
    return n;
}
