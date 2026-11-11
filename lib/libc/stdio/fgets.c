/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <string.h>
#include "local.h"

char *fgets(char *__restrict buf, int n, FILE *__restrict fp)
{
    size_t len;
    size_t remain;
    char *s;
    unsigned char *p, *t;

    if (n <= 0)
        return NULL;

    ORIENT(fp, -1);

    FLOCKFILE(fp);
    s = buf;
    remain = (size_t) (n - 1);
    while (remain != 0) {
        if (fp->_r <= 0) {
            if (__srefill(fp))
                break;
        }
        len = (size_t) fp->_r;
        p = fp->_p;
        if (remain < len)
            len = remain;
        t = memchr((void *) p, '\n', len);
        if (t) {
            len = (size_t) (++t - p);
            fp->_r -= (int) len;
            fp->_p = t;
            (void) memcpy((void *) s, (void *) p, len);
            s[len] = 0;
            FUNLOCKFILE(fp);
            return buf;
        }
        fp->_r -= (int) len;
        fp->_p += len;
        (void) memcpy((void *) s, (void *) p, len);
        s += len;
        remain -= len;
    }
    *s = 0;
    FUNLOCKFILE(fp);
    if (s == buf)
        return NULL;
    return buf;
}
