/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <sys/dispatch.h>
#include "api.h"
#include "internal.h"
#include "dispatch/state.h"

int ioserv_backend_msg_again(ioserv_context_t *ctp, int rcvid)
{
    if (msg_info(rcvid, &ctp->info) == -1) {
        return -1;
    }

    if ((int) (ctp->info.msglen = (uint32_t) msg_read(
                   ctp->rcvid = rcvid, ctp->msg, (int) ctp->msg_max_size, 0)) ==
        -1) {
        return -1;
    }

    (void) msg_current(rcvid);

    _ioserv_handler(ctp);

    return 0;
}
