/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <errno.h>
#include <stdio.h>
#include "local.h"

/* Locking flush for the fflush(NULL) walk: each stream is flushed under its own
 * lock so the all-streams flush is thread-safe per stream.
 */
static int lflush(FILE *fp)
{
    int r;

    FLOCKFILE(fp);
    r = __sflush(fp);
    FUNLOCKFILE(fp);
    return r;
}

int fflush(FILE *fp)
{
    int retval;

    if (!fp)
        return _fwalk(lflush);

    FLOCKFILE(fp);
    if ((fp->_flags & (__SWR | __SRW)) == 0) {
        FUNLOCKFILE(fp);
        errno = EBADF;
        return EOF;
    }

    retval = __sflush(fp);
    FUNLOCKFILE(fp);
    return retval;
}

/* Internal flush — no locking */
int __fflush(FILE *fp)
{
    return __sflush(fp);
}

int __sflush(FILE *fp)
{
    unsigned char *p;
    int n, t;

    t = fp->_flags;
    if ((t & __SWR) == 0)
        return 0;

    if (!(p = fp->_bf._base))
        return 0;

    n = (int) (fp->_p - p);
    fp->_p = p;
    fp->_w = t & (__SLBF | __SNBF) ? 0 : fp->_bf._size;

    for (; n > 0; n -= t, p += t) {
        t = _swrite(fp, (char *) p, n);
        if (t <= 0) {
            /* Reset _p even on error */
            if (t < 0 && __sseek(fp, (fpos_t) 0, SEEK_CUR) >= 0)
                ;
            fp->_flags |= __SERR;
            return EOF;
        }
    }
    return 0;
}
