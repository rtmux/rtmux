/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/rtmux-core.h>
#include <sys/ioserv-private.h>
#include "api.h"

int ioserv_backend_msgwrite(ioserv_context_t *ctp,
                            const void *msg,
                            int nbytes,
                            int offset)
{
    return msg_write(ctp->rcvid, msg, nbytes, offset);
}
