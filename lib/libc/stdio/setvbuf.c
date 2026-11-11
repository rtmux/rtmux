/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include "local.h"

int setvbuf(FILE *__restrict fp, char *__restrict buf, int mode, size_t size)
{
    int ret;
    unsigned flags;

    ret = 0;

    (void) __sflush(fp);
    if (fp->_flags & __SMBF)
        free((void *) fp->_bf._base);
    fp->_w = 0;
    fp->_lbfsize = 0;

    flags =
        fp->_flags & ~(unsigned) (__SLBF | __SNBF | __SMBF | __SOPT | __SNPT);

    switch (mode) {
    case _IONBF:
        flags |= __SNBF;
        fp->_bf._base = fp->_p = fp->_nbuf;
        fp->_bf._size = 1;
        break;
    case _IOLBF:
        flags |= __SLBF;
        /* FALLTHROUGH */
    case _IOFBF:
        /* allocate buffer if needed */
        if (!buf) {
            if (size == 0) {
                if (fp->_blksize > 0)
                    size = (size_t) fp->_blksize;
                else
                    size = BUFSIZ;
            }
            if (!(buf = malloc(size))) {
                ret = EOF;
                break;
            }
            flags |= __SMBF;
        }
        fp->_bf._base = fp->_p = (unsigned char *) buf;
        fp->_bf._size = (int) size;
        break;
    default:
        ret = EOF;
        break;
    }

    if (ret == 0) {
        fp->_flags = (unsigned short) flags;
        if (flags & __SLBF)
            fp->_lbfsize = -fp->_bf._size;
        fp->_r = 0;
    }
    return ret;
}

void setbuf(FILE *__restrict fp, char *__restrict buf)
{
    (void) setvbuf(fp, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}
