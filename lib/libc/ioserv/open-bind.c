/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <errno.h>
#include <sys/rtmux-core.h>
#include <sys/ioserv-private.h>
#include "api.h"
#include "internal.h"

int ioserv_backend_open_bind(ioserv_context_t *ctp,
                             void *ocb,
                             const ioserv_ops_t *funcs)
{
    struct binding *binding;
    struct link *link;

    if (!(link = _ioserv_link_query(ctp->id, 1))) {
        errno = ENOENT;
        return -1;
    }

    if (!(binding = calloc(sizeof *binding, 1))) {
        errno = ENOMEM;
        return -1;
    }
    binding->count = 1;
    binding->id = link->id;
    binding->ocb = ocb;
    binding->funcs = funcs ? funcs : link->io_funcs;

    _ioserv_link_return(link, 1);

    if (_ioserv_handle(&ctp->info, binding, _IOSERV_HANDLE_SET) ==
        (void *) -1) {
        free(binding);
        errno = EINVAL;
        return -1;
    }
    return 0;
}
