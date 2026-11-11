/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <errno.h>
#include <sys/ioserv-private.h>
#include <sys/rtmux-core.h>
#include <atomic.h>
#include "internal.h"

int _ioserv_unbind(struct _msg_info *rep)
{
    struct binding *binding;

    if ((binding = _ioserv_handle(rep, NULL, _IOSERV_HANDLE_REMOVE_LOCK)) ==
        (void *) -1)
        return -1;

    if (atomic_sub_value(&binding->count, 1) == 1)
        free(binding);


    return 0;
}
