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
        _ioserv_close_binding(ctp, msg, binding);
    }
    __connect_detach((uint64_t) scoid);
    return IOSERV_NOREPLY;
}
