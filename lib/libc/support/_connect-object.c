/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <alloca.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/iomsg.h>
#include "connect.h"

int _connect_object(const char *name,
                    const char *prefix,
                    mode_t mode,
                    int oflag,
                    unsigned file_type,
                    unsigned extra_type,
                    unsigned extra_len,
                    const void *extra)
{
    char *path;

    if (*name != '/' || strchr(name + 1, '/')) {
        path = (char *) name;
    } else {
        path = (char *) alloca(strlen(prefix) + strlen(name) + 1);
        if (!path) {
            errno = ENOMEM;
            return -1;
        }
        strcat(strcpy(path, prefix), name);
    }

    if (oflag & O_CREAT)
        mode &= ~(mode_t) S_IFMT;
    else {
        mode = 0;
        extra = 0;
    }

    return _connect(0, path, mode, (unsigned) oflag | O_NOCTTY, 0,
                    _IO_CONNECT_OPEN, 0, _IO_FLAG_RD | _IO_FLAG_WR, file_type,
                    extra ? extra_type : 0, extra ? extra_len : 0, extra, 0, 0,
                    0);
}
