/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/ioserv-private.h>
#include "internal.h"

_ioserv_func_t _ioserv_io_func(const ioserv_ops_t *funcs, unsigned type)
{
    if (funcs) {
        if (type >= _IO_READ) {
            type -= _IO_READ;
            if (type < funcs->nfuncs) {
                return *(((_ioserv_func_t *) (&funcs->read)) + type);
            }
        }
    }
    return 0;
}
