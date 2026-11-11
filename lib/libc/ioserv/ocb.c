/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/ioserv-private.h>
#include "internal.h"

void *_ioserv_ocb(ioserv_context_t *ctp, struct _msg_info *info)
{
    struct binding *binding;

    if ((binding = (struct binding *) _ioserv_handle(
             info, 0, _IOSERV_HANDLE_FIND)) == (void *) -1) {
        return 0;
    }
    return binding->ocb;
}
