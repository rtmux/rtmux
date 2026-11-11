/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local.h"

static int __submore(FILE *fp)
{
    int i;
    unsigned char *p;

    if (fp->_ub._base == fp->_ubuf) {
        if (!(p = malloc((size_t) BUFSIZ)))
            return EOF;
        fp->_ub._base = p;
        fp->_ub._size = BUFSIZ;
        p += BUFSIZ - sizeof(fp->_ubuf);
        for (i = sizeof(fp->_ubuf); --i >= 0;)
            p[i] = fp->_ubuf[i];
        fp->_p = p;
        return 0;
    }
    i = fp->_ub._size;
    p = realloc(fp->_ub._base, (size_t) (i << 1));
    if (!p)
        return EOF;
    (void) memcpy((void *) (p + i), (void *) p, (size_t) i);
    fp->_p = p + i;
    fp->_ub._base = p;
    fp->_ub._size = i << 1;
    return 0;
}

int __ungetc(int c, FILE *fp)
{
    if (c == EOF)
        return EOF;
    if ((fp->_flags & __SRD) == 0) {
        if ((fp->_flags & __SRW) == 0)
            return EOF;
        if (fp->_flags & __SWR) {
            if (__sflush(fp))
                return EOF;
            __sclear(fp, __SWR);
            fp->_w = 0;
            fp->_lbfsize = 0;
        }
        fp->_flags |= __SRD;
    }
    c = (unsigned char) c;

    if (fp->_bf._base && fp->_p > fp->_bf._base && fp->_p[-1] == c) {
        fp->_p--;
        fp->_r++;
        __sclear(fp, __SEOF);
        return c;
    }

    if (!fp->_ub._base) {
        fp->_ub._base = fp->_ubuf;
        fp->_ub._size = sizeof(fp->_ubuf);
    }

    if (HASUB(fp)) {
        if (fp->_r >= fp->_ub._size && __submore(fp))
            return EOF;
    } else {
        fp->_ur = fp->_r;
        fp->_up = fp->_p;
        fp->_ub._base = fp->_ubuf;
        fp->_ub._size = sizeof(fp->_ubuf);
        fp->_r = 0;
    }
    fp->_p = fp->_ub._base + fp->_ub._size - 1 - fp->_r;
    *(fp->_p) = (unsigned char) c;
    fp->_r++;
    __sclear(fp, __SEOF);
    return c;
}

int ungetc(int c, FILE *fp)
{
    int r;

    FLOCKFILE(fp);
    r = __ungetc(c, fp);
    FUNLOCKFILE(fp);
    return r;
}
