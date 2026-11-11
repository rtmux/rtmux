/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: removed reallocf (use realloc).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "local.h"
#include "fvwrite.h"

/* Write some memory regions.
 *
 * Return zero on success, EOF on error.
 */
int __sfvwrite(FILE *fp, struct __suio *uio)
{
    size_t len;
    size_t nw; /* validated non-negative write count */
    char *p;
    struct __siov *iov;
    int w, s;
    char *nl;
    int nlknown, nldist;

    if (uio->uio_resid == 0)
        return 0;
    if (prepwrite(fp) != 0)
        return EOF;

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define COPY(n) (void) memcpy((void *) fp->_p, (void *) p, (size_t) (n))

    iov = uio->uio_iov;
    p = iov->iov_base;
    len = iov->iov_len;
    iov++;
#define GETIOV(extra_work)  \
    while (len == 0) {      \
        extra_work;         \
        p = iov->iov_base;  \
        len = iov->iov_len; \
        iov++;              \
    }
    if (fp->_flags & __SNBF) {
        do {
            GETIOV(;);
            w = _swrite(fp, p, (int) MIN(len, BUFSIZ));
            if (w <= 0)
                goto err;
            nw = (size_t) w;
            p += nw;
            len -= nw;
        } while ((uio->uio_resid -= nw) != 0);
    } else if ((fp->_flags & __SLBF) == 0) {
        do {
            GETIOV(;);
            if ((fp->_flags & (__SALC | __SSTR)) == (__SALC | __SSTR) &&
                fp->_w < (int) len) {
                size_t blen = (size_t) (fp->_p - fp->_bf._base);
                size_t newsize = blen + len + 128;
                fp->_w = (int) (len + 128);
                fp->_bf._size = (int) newsize;
                fp->_bf._base = realloc(fp->_bf._base, newsize + 1);
                if (!fp->_bf._base)
                    goto err;
                fp->_p = fp->_bf._base + blen;
            }
            w = fp->_w;
            if (fp->_flags & __SSTR) {
                if ((int) len < w)
                    w = (int) len;
                if (w > 0) {
                    COPY(w);
                    fp->_w -= w;
                    fp->_p += w;
                }
                w = (int) len;
            } else if (fp->_p > fp->_bf._base && (int) len > w) {
                COPY(w);
                fp->_p += w;
                if (__fflush(fp))
                    goto err;
            } else if ((int) len >= (w = fp->_bf._size)) {
                w = _swrite(fp, p, w);
                if (w <= 0)
                    goto err;
            } else {
                w = (int) len;
                COPY(w);
                fp->_w -= w;
                fp->_p += w;
            }
            nw = (size_t) w;
            p += nw;
            len -= nw;
        } while ((uio->uio_resid -= nw) != 0);
    } else {
        nlknown = 0;
        nldist = 0;
        do {
            GETIOV(nlknown = 0);
            if (!nlknown) {
                nl = memchr((void *) p, '\n', len);
                nldist = nl ? (int) (nl + 1 - p) : (int) (len + 1);
                nlknown = 1;
            }
            s = (int) MIN(len, (size_t) nldist);
            w = fp->_w + fp->_bf._size;
            if (fp->_p > fp->_bf._base && s > w) {
                COPY(w);
                fp->_p += w;
                if (__fflush(fp))
                    goto err;
            } else if (s >= (w = fp->_bf._size)) {
                w = _swrite(fp, p, w);
                if (w <= 0)
                    goto err;
            } else {
                w = s;
                COPY(w);
                fp->_w -= w;
                fp->_p += w;
            }
            if ((nldist -= w) == 0) {
                if (__fflush(fp))
                    goto err;
                nlknown = 0;
            }
            nw = (size_t) w;
            p += nw;
            len -= nw;
        } while ((uio->uio_resid -= nw) != 0);
    }
    return 0;

err:
    fp->_flags |= __SERR;
    return EOF;
}
