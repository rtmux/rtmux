/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: removed threading/cancellation.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "local.h"

static int lflush(FILE *fp)
{
    if ((fp->_flags & (__SLBF | __SWR)) == (__SLBF | __SWR))
        return __sflush(fp);
    return 0;
}

/* Refill a stdio buffer.
 * Return EOF on eof or error, 0 otherwise.
 */
int __srefill(FILE *fp)
{
    if (!__sdidinit)
        __sinit();

    ORIENT(fp, -1);

    fp->_r = 0;

    if (fp->_flags & __SEOF)
        return EOF;

    if ((fp->_flags & __SRD) == 0) {
        if ((fp->_flags & __SRW) == 0) {
            errno = EBADF;
            fp->_flags |= __SERR;
            return EOF;
        }
        if (fp->_flags & __SWR) {
            if (__sflush(fp))
                return EOF;
            __sclear(fp, __SWR);
            fp->_w = 0;
            fp->_lbfsize = 0;
        }
        fp->_flags |= __SRD;
    } else {
        if (HASUB(fp)) {
            FREEUB(fp);
            if ((fp->_r = fp->_ur) != 0) {
                fp->_p = fp->_up;
                return 0;
            }
        }
    }

    if (!fp->_bf._base)
        __smakebuf(fp);

    if (fp->_flags & (__SLBF | __SNBF)) {
        fp->_flags |= __SIGN;
        (void) _fwalk(lflush);
        __sclear(fp, __SIGN);

        if ((fp->_flags & (__SLBF | __SWR)) == (__SLBF | __SWR))
            __sflush(fp);
    }

    fp->_p = fp->_bf._base;
    fp->_r = _sread(fp, (char *) fp->_p, fp->_bf._size);
    if (fp->_r <= 0) {
        if (fp->_r == 0)
            fp->_flags |= __SEOF;
        else {
            fp->_r = 0;
            fp->_flags |= __SERR;
        }
        return EOF;
    }
    return 0;
}
