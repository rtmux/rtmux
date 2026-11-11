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

FILE *fopen(const char *__restrict file, const char *__restrict mode)
{
    FILE *fp;
    int f;
    int flags, oflags;

    if ((flags = __sflags(mode, &oflags)) == 0)
        return NULL;
    if (!(fp = __sfp()))
        return NULL;
    if ((f = open(file, oflags, 0666)) < 0) {
        fp->_flags = 0;
        return NULL;
    }

    if (oflags & O_APPEND)
        fp->_flags2 |= __S2OAP;
    fp->_file = (short) f;
    fp->_flags = (unsigned short) flags;
    fp->_cookie = fp;
    fp->_read = __sread;
    fp->_write = __swrite;
    fp->_seek = __sseek;
    fp->_close = __sclose;

    return fp;
}
