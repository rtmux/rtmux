/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <sys/rtmux-core.h>
#include <atomic.h>
#include <errno.h>
#include <sys/iomsg.h>
#include <sys/ioserv-private.h>
#include "internal.h"

/* Close binding as a close message from its client would, with no reply:
 * msg (the request just handled) is rewritten into that close.
 */
void _ioserv_close_binding(ioserv_context_t *ctp,
                           ioserv_msg_t *msg,
                           struct binding *binding)
{
    int rcvid = ctp->rcvid;

    ctp->rcvid = 0;
    ctp->info.msglen = sizeof msg->close;
    msg->close.i.type = _IO_CLOSE;
    msg->close.i.combine_len = sizeof msg->close.i;
    (void) _ioserv_io_handler(ctp, msg, binding);
    ctp->rcvid = rcvid;
}

void _ioserv_close_handler(ioserv_context_t *ctp, struct binding *binding)
{
    if (atomic_sub_value(&binding->count, 1) == 1) {
        _ioserv_func_t func;

        if ((func = _ioserv_io_func(binding->funcs, _IO_RSVD_CLOSE_OCB))) {
            int id;

            id = ctp->rcvid;
            ctp->rcvid = 0;
            (void) func(ctp, 0, binding->ocb);
            ctp->rcvid = id;
        }
        free(binding);
    }
}
