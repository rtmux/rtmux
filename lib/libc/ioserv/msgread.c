/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/rtmux-core.h>
#include <sys/ioserv-private.h>
#include "api.h"

int ioserv_backend_msgread(ioserv_context_t *ctp,
                           void *msg,
                           int nbytes,
                           int offset)
{
    return msg_read(ctp->rcvid, msg, nbytes, ctp->offset + offset);
}
