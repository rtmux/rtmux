/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#undef _FILE_OFFSET_BITS
#define _FILE_OFFSET_BITS 64

#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioserv-private.h>
#include "api.h"
#include "internal.h"

int ioserv_backend_devino(int id, dev_t *pdevno, ino_t *pino)
{
    struct link *p;
    struct stat buff;

    if (!(p = _ioserv_link_query(id, 1)) || fstat(p->link_id, &buff) == -1) {
        if (p) {
            _ioserv_link_return(p, 1);
        }
        errno = EINVAL;
        return -1;
    }
    _ioserv_link_return(p, 1);
    *pdevno = buff.st_dev;
    *pino = buff.st_ino;
    return 0;
}
