/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/rtmux-core.h>
#include <sys/ioserv-private.h>
#include "api.h"

int ioserv_backend_msgwritev(ioserv_context_t *ctp,
                             const struct iovec *smsg,
                             int sparts,
                             int offset)
{
    return msg_writev(ctp->rcvid, smsg, sparts, offset);
}
