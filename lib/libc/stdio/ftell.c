/*
 * ftell, ftello — return current file position
 *
 * From FreeBSD lib/libc/stdio/ftell.c
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 *     The Regents of the University of California.  All rights reserved.
 * This code is derived from software contributed to Berkeley by Chris Torek.
 */

#include <sys/types.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include "local.h"

#ifndef OFF_MAX
#define OFF_MAX LONG_MAX
#endif

long ftell(FILE *fp)
{
    off_t rv;

    rv = ftello(fp);
    if (rv > LONG_MAX) {
        errno = EOVERFLOW;
        return -1;
    }
    return rv;
}

off_t ftello(FILE *fp)
{
    fpos_t rv;
    int ret;

    FLOCKFILE(fp);
    ret = _ftello(fp, &rv);
    FUNLOCKFILE(fp);
    if (ret)
        return -1;
    if (rv < 0) {
        errno = ESPIPE;
        return -1;
    }
    return rv;
}

int _ftello(FILE *fp, fpos_t *offset)
{
    fpos_t pos;
    fpos_t n;

    if (!fp->_seek) {
        errno = ESPIPE;
        return 1;
    }

    /* Find offset of underlying I/O object, then adjust for buffered bytes. */
    if (!(fp->_flags & __SRD) && (fp->_flags & __SWR) && fp->_p &&
        fp->_p - fp->_bf._base > 0 &&
        ((fp->_flags & __SAPP) || (fp->_flags2 & __S2OAP))) {
        pos = _sseek(fp, (fpos_t) 0, SEEK_END);
        if (pos == -1)
            return 1;
    } else if (fp->_flags & __SOFF)
        pos = fp->_offset;
    else {
        pos = _sseek(fp, (fpos_t) 0, SEEK_CUR);
        if (pos == -1)
            return 1;
    }
    if (fp->_flags & __SRD) {
        /* Reading. Any unread characters (including those from ungetc) cause
         * the position to be smaller than that in the underlying object.
         */
        if ((pos -= (HASUB(fp) ? fp->_ur : fp->_r)) < 0) {
            fp->_flags |= __SERR;
            errno = EIO;
            return 1;
        }
        if (HASUB(fp))
            pos -= fp->_r;
    } else if ((fp->_flags & __SWR) && fp->_p &&
               (n = fp->_p - fp->_bf._base) > 0) {
        /* Writing. Any buffered characters cause the position to be greater
         * than that in the underlying object.
         */
        if (pos > OFF_MAX - n) {
            errno = EOVERFLOW;
            return 1;
        }
        pos += n;
    }
    *offset = pos;
    return 0;
}
