/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/ioserv-private.h>
#include "internal.h"

const ioserv_ops_t *_ioserv_binding_funcs(ioserv_context_t *ctp,
                                          struct _msg_info *info)
{
    struct binding *binding;

    binding = (struct binding *) _ioserv_handle(info, 0, _IOSERV_HANDLE_FIND);
    if (binding == (void *) -1)
        return NULL;
    return binding->funcs;
}
