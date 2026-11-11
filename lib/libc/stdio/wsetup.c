/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include "local.h"

/* Various output routines call wsetup to be sure it is safe to write, because
 * either _flags does not include __SWR, or _buf is NULL. __swsetup returns 0 if
 * OK to write; otherwise, it returns EOF and sets errno.
 */
int __swsetup(FILE *fp)
{
    if (!__sdidinit)
        __sinit();

    if ((fp->_flags & __SWR) == 0) {
        if ((fp->_flags & __SRW) == 0) {
            errno = EBADF;
            fp->_flags |= __SERR;
            return EOF;
        }
        if (fp->_flags & __SRD) {
            if (HASUB(fp))
                FREEUB(fp);
            __sclear(fp, __SRD | __SEOF);
            fp->_r = 0;
            fp->_p = fp->_bf._base;
        }
        fp->_flags |= __SWR;
    }

    if (!fp->_bf._base)
        __smakebuf(fp);
    if (fp->_flags & __SLBF) {
        fp->_w = 0;
        fp->_lbfsize = -fp->_bf._size;
    } else
        fp->_w = fp->_flags & __SNBF ? 0 : fp->_bf._size;
    return 0;
}
