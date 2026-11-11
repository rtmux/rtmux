/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: removed namespace wrappers.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "local.h"

extern void (*__cleanup)(void) __hidden;

/* Allocate a file buffer, or switch to unbuffered I/O. Per the ANSI C standard,
 * ALL tty devices default to line buffered.
 */
void __smakebuf(FILE *fp)
{
    void *p;
    unsigned flags;
    size_t size;
    int couldbetty;

    if (fp->_flags & __SNBF) {
        fp->_bf._base = fp->_p = fp->_nbuf;
        fp->_bf._size = 1;
        return;
    }
    flags = __swhatbuf(fp, &size, &couldbetty);
    if (!(p = malloc(size))) {
        fp->_flags |= __SNBF;
        fp->_bf._base = fp->_p = fp->_nbuf;
        fp->_bf._size = 1;
        return;
    }
    __cleanup = _cleanup;
    flags |= __SMBF;
    fp->_bf._base = fp->_p = p;
    fp->_bf._size = (int) size;
    if (couldbetty && isatty(fp->_file))
        flags |= __SLBF;
    fp->_flags |= (unsigned short) flags;
}

/* Internal routine to determine "proper' buffering for a file. */
unsigned __swhatbuf(FILE *fp, size_t *bufsize, int *couldbetty)
{
    struct stat st;

    if (fp->_file < 0 || fstat(fp->_file, &st) < 0) {
        *couldbetty = 0;
        *bufsize = BUFSIZ;
        return __SNPT;
    }

    *couldbetty = (st.st_mode & S_IFMT) == S_IFCHR;
    if (st.st_blksize <= 0) {
        *bufsize = BUFSIZ;
        return __SNPT;
    }

    *bufsize = st.st_blksize;
    fp->_blksize = (int) st.st_blksize;
    return ((st.st_mode & S_IFMT) == S_IFREG && fp->_seek == __sseek ? __SOPT
                                                                     : __SNPT);
}
