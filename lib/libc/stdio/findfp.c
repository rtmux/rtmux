/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: removed threading, glue lists, simplified pool.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "local.h"

int __sdidinit;
__hidden int __isthreaded;

void (*__cleanup)(void) __hidden;

#define NDYNAMIC 10

#define std(flags, file)          \
    {                             \
        ._flags = (flags),        \
        ._file = (file),          \
        ._cookie = __sF + (file), \
        ._close = __sclose,       \
        ._read = __sread,         \
        ._seek = __sseek,         \
        ._write = __swrite,       \
    }

static FILE usual[FOPEN_MAX - 3];

static FILE __sF[3] = {
    [STDIN_FILENO] = std(__SRD, STDIN_FILENO),
    [STDOUT_FILENO] = std(__SWR, STDOUT_FILENO),
    [STDERR_FILENO] = std(__SWR | __SNBF, STDERR_FILENO),
};

FILE *stdin = &__sF[0];
FILE *stdout = &__sF[1];
FILE *stderr = &__sF[2];
FILE *__stdinp = &__sF[0];
FILE *__stdoutp = &__sF[1];
FILE *__stderrp = &__sF[2];

/* Find a free FILE for fopen et al. */
FILE *__sfp(void)
{
    FILE *fp;
    int n;

    if (!__sdidinit)
        __sinit();

    /* Search the static pool first */
    for (fp = usual, n = FOPEN_MAX - 3; --n >= 0; fp++)
        if (fp->_flags == 0)
            goto found;

    /* No free slots */
    return NULL;

found:
    fp->_flags = 1; /* reserve this slot */
    fp->_p = NULL;
    fp->_w = 0;
    fp->_r = 0;
    fp->_bf._base = NULL;
    fp->_bf._size = 0;
    fp->_lbfsize = 0;
    fp->_file = -1;
    fp->_ub._base = NULL;
    fp->_ub._size = 0;
    fp->_flags2 = 0;
    return fp;
}

/* exit() calls _cleanup() through *__cleanup. */
void _cleanup(void)
{
    (void) _fwalk(__sflush);
}

/* Walk all open stdio files and call the given function. */
int _fwalk(int (*func)(FILE *))
{
    FILE *fp;
    int n, ret;

    ret = 0;

    /* Walk the three standard streams */
    for (fp = __sF, n = 3; --n >= 0; fp++)
        if (fp->_flags != 0 && !(fp->_flags & __SIGN))
            ret |= (*func)(fp);

    /* Walk the pool */
    for (fp = usual, n = FOPEN_MAX - 3; --n >= 0; fp++)
        if (fp->_flags != 0 && !(fp->_flags & __SIGN))
            ret |= (*func)(fp);

    return ret;
}

/* __sinit() is called whenever stdio's internal variables must be set up. */
void __sinit(void)
{
    __cleanup = _cleanup;
    __sdidinit = 1;
}
