/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: removed namespace wrappers.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "local.h"

#ifndef OFF_MAX
#define OFF_MAX __LONG_MAX__
#endif

/* Small standard I/O/seek/close functions. */
int __sread(void *cookie, char *buf, int n)
{
    FILE *fp = cookie;
    return (int) (read(fp->_file, buf, (size_t) n));
}

int __swrite(void *cookie, char const *buf, int n)
{
    FILE *fp = cookie;
    return (int) (write(fp->_file, buf, (size_t) n));
}

fpos_t __sseek(void *cookie, fpos_t offset, int whence)
{
    FILE *fp = cookie;
    return lseek(fp->_file, (off_t) offset, whence);
}

int __sclose(void *cookie)
{
    return close(((FILE *) cookie)->_file);
}

/* Higher level wrappers. */
int _sread(FILE *fp, char *buf, int n)
{
    int ret;

    ret = (*fp->_read)(fp->_cookie, buf, n);
    if (ret > 0) {
        if (fp->_flags & __SOFF) {
            if (fp->_offset <= OFF_MAX - ret)
                fp->_offset += ret;
            else
                __sclear(fp, __SOFF);
        }
    } else if (ret < 0)
        __sclear(fp, __SOFF);
    return ret;
}

int _swrite(FILE *fp, char const *buf, int n)
{
    int ret;
    int serrno;

    if (fp->_flags & __SAPP) {
        serrno = errno;
        if (_sseek(fp, (fpos_t) 0, SEEK_END) == -1 && (fp->_flags & __SOPT))
            return -1;
        errno = serrno;
    }
    ret = (*fp->_write)(fp->_cookie, buf, n);
    if (ret >= 0) {
        if ((fp->_flags & __SOFF) && !(fp->_flags2 & __S2OAP) &&
            fp->_offset <= OFF_MAX - ret)
            fp->_offset += ret;
        else
            __sclear(fp, __SOFF);
    } else if (ret < 0)
        __sclear(fp, __SOFF);
    return ret;
}

fpos_t _sseek(FILE *fp, fpos_t offset, int whence)
{
    fpos_t ret;
    int serrno, errret;

    serrno = errno;
    errno = 0;
    ret = (*fp->_seek)(fp->_cookie, offset, whence);
    errret = errno;
    if (errno == 0)
        errno = serrno;
    if (ret < 0) {
        if (errret == 0) {
            if (offset != 0 || whence != SEEK_CUR) {
                if (HASUB(fp))
                    FREEUB(fp);
                fp->_p = fp->_bf._base;
                fp->_r = 0;
                __sclear(fp, __SEOF);
            }
            fp->_flags |= __SERR;
            errno = EINVAL;
        }
        __sclear(fp, __SOFF);
        ret = -1;
    } else if (fp->_flags & __SOPT) {
        fp->_flags |= __SOFF;
        fp->_offset = ret;
    }
    return ret;
}
