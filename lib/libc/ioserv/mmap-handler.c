/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/memmsg.h>
#include <stddef.h>
#include <unistd.h>
#include <atomic.h>
#include <sys/ioserv-private.h>
#include <sys/rtmux-core.h>
#include "internal.h"

int _ioserv_mmap_handler(ioserv_context_t *ctp, ioserv_msg_t *msg)
{
    int n;
    struct binding *binding;

    if (ctp->info.pid != ROOTTASK_PID) {
        return ENOSYS;
    }

    if ((binding = (struct binding *) _ioserv_handle(
             &msg->mmap.i.info, 0, _IOSERV_HANDLE_FIND_LOCK)) == (void *) -1) {
        return EBADF;
    }

    atomic_add(&binding->count, 1);
    _ioserv_handle(&msg->mmap.i.info, 0, _IOSERV_HANDLE_UNLOCK);

    _ioserv_handle(&ctp->info, 0, _IOSERV_HANDLE_FIND_LOCK);

    n = _ioserv_io_handler(ctp, msg, binding);

    _ioserv_close_handler(ctp, binding);

    return n;
}
