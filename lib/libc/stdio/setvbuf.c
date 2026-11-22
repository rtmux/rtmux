/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include "local.h"

int setvbuf(FILE *__restrict fp, char *__restrict buf, int mode, size_t size)
{
    int ret = 0;
    unsigned flags;

    if ((mode != _IONBF && mode != _IOLBF && mode != _IOFBF) ||
        (mode != _IONBF && (size > INT_MAX || (buf && !size)))) {
        errno = EINVAL;
        return EOF;
    }

    FLOCKFILE(fp);
    if (__sflush(fp) != 0) {
        FUNLOCKFILE(fp);
        return EOF;
    }
    flags = fp->_flags & ~(unsigned) (__SLBF | __SNBF | __SMBF | __SOPT | __SNPT);
    if (mode != _IONBF && !buf) {
        if (!size)
            size = fp->_blksize > 0 ? (size_t) fp->_blksize : BUFSIZ;
        buf = malloc(size);
        if (buf)
            flags |= __SMBF;
        else {
            mode = _IONBF;
            ret = EOF;
        }
    }

    if (fp->_flags & __SMBF)
        free(fp->_bf._base);
    if (HASUB(fp))
        FREEUB(fp);
    if (mode == _IONBF) {
        flags |= __SNBF;
        buf = (char *) fp->_nbuf;
        size = 1;
    } else if (mode == _IOLBF) {
        flags |= __SLBF;
    }
    fp->_flags = (unsigned short) flags;
    fp->_bf._base = fp->_p = (unsigned char *) buf;
    fp->_bf._size = (int) size;
    fp->_r = 0;
    fp->_lbfsize = (flags & __SLBF) ? -(int) size : 0;
    fp->_w = (flags & __SWR) && mode == _IOFBF ? (int) size : 0;
    FUNLOCKFILE(fp);
    return ret;
}

void setbuf(FILE *__restrict fp, char *__restrict buf)
{
    (void) setvbuf(fp, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}
