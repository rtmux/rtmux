/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 */

#include <errno.h>
#include <stdio.h>
#include "local.h"

/* Write the given character into the (probably full) buffer for the given file.
 * Flush the buffer out if it is or becomes full, or if c=='\n' and the file is
 * line buffered.
 */
int __swbuf(int c, FILE *fp)
{
    int n;

    fp->_w = fp->_lbfsize;
    if (prepwrite(fp) != 0) {
        errno = EBADF;
        return EOF;
    }
    c = (unsigned char) c;

    ORIENT(fp, -1);

    n = (int) (fp->_p - fp->_bf._base);
    if (n >= fp->_bf._size) {
        if (__fflush(fp) != 0)
            return EOF;
        n = 0;
    }
    fp->_w--;
    *fp->_p++ = (unsigned char) c;
    if (++n == fp->_bf._size || (fp->_flags & __SLBF && c == '\n'))
        if (__fflush(fp) != 0)
            return EOF;
    return c;
}
