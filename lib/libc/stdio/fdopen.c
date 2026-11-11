/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include "local.h"

FILE *fdopen(int fd, const char *mode)
{
    FILE *fp;
    int flags, oflags, fdflags, tmp;

    if ((flags = __sflags(mode, &oflags)) == 0)
        return NULL;

    /* Make sure the mode the user wants is a subset of the actual mode. */
    if ((fdflags = fcntl(fd, F_GETFL, 0)) < 0)
        return NULL;
    tmp = fdflags & O_ACCMODE;
    if (tmp != O_RDWR && (tmp != (oflags & O_ACCMODE))) {
        errno = EINVAL;
        return NULL;
    }

    if (!(fp = __sfp()))
        return NULL;
    fp->_flags = (unsigned short) flags;
    if (oflags & O_APPEND)
        fp->_flags2 |= __S2OAP;

    if (flags & __SWR)
        fp->_flags |= __SAPP;

    fp->_file = (short) fd;
    fp->_cookie = fp;
    fp->_read = __sread;
    fp->_write = __swrite;
    fp->_seek = __sseek;
    fp->_close = __sclose;
    return fp;
}
