/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/ioserv.h>
#include <sys/ioserv-private.h>
#include "internal.h"

int _ioserv_disconnect_handler(ioserv_context_t *ctp,
                               ioserv_msg_t *msg,
                               int scoid)
{
    struct binding *binding;

    ioserv_reply_counters_record_disconnect();
    ctp->info.scoid = scoid;
    while ((binding = (struct binding *) _ioserv_handle(
                &ctp->info, 0, _IOSERV_HANDLE_DISCONNECT_LOCK)) !=
           (void *) -1) {
        ctp->info.msglen = sizeof msg->close;
        msg->close.i.type = _IO_CLOSE;
        msg->close.i.combine_len = sizeof msg->close.i;
        (void) _ioserv_io_handler(ctp, msg, binding);
    }
    __connect_detach((uint64_t) scoid);
    return IOSERV_NOREPLY;
}
