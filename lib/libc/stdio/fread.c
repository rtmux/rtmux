/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <stdio.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "local.h"

size_t fread(void *__restrict buf,
             size_t size,
             size_t count,
             FILE *__restrict fp)
{
    size_t r;

    FLOCKFILE(fp);
    r = __fread(buf, size, count, fp);
    FUNLOCKFILE(fp);
    return r;
}

size_t __fread(void *__restrict buf,
               size_t size,
               size_t count,
               FILE *__restrict fp)
{
    size_t resid;
    char *p;
    int r;
    size_t total;

    if (size != 0 && count > SIZE_MAX / size) {
        errno = EOVERFLOW;
        fp->_flags |= __SERR;
        return 0;
    }
    if ((resid = count * size) == 0)
        return 0;

    ORIENT(fp, -1);

    p = buf;
    total = resid;
    while (resid > 0) {
        size_t nr;
        r = fp->_r;
        if (r <= 0) {
            if (__srefill(fp))
                break;
            r = fp->_r;
        }
        nr = (size_t) r > resid ? resid : (size_t) r;
        (void) memcpy((void *) p, (void *) fp->_p, nr);
        fp->_r -= (int) nr;
        fp->_p += nr;
        p += nr;
        resid -= nr;
    }
    return (total - resid) / size;
}
