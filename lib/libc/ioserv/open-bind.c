/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <errno.h>
#include <stdatomic.h>
#include <sys/rtmux-core.h>
#include <sys/ioserv-private.h>
#include "api.h"
#include "internal.h"

/* A serial for each new binding, so that one freed and another made at the same
 * address are not taken for each other.
 */
static unsigned binding_serial(void)
{
    static atomic_uint next;

    return atomic_fetch_add_explicit(&next, 1, memory_order_relaxed) + 1;
}

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
        _ioserv_link_return(link, 1);
        errno = ENOMEM;
        return -1;
    }
    binding->count = 1;
    binding->serial = binding_serial();
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
    ctp->bound = binding;
    ctp->bound_serial = binding->serial;
    return 0;
}
