/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 *     The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by Chris Torek.
 *
 * Adapted for RTMux: removed namespace wrappers and mbstate.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include "local.h"

#ifndef OFF_MAX
#define OFF_MAX LONG_MAX
#endif

#define POS_ERR (-(fpos_t) 1)

/* Seek the given file to the given offset. "Whence' must be one of the three
 * SEEK_* macros.
 */
static int _fseeko(FILE *fp, off_t offset, int whence, int ltest)
{
    fpos_t (*seekfn)(void *, fpos_t, int);
    fpos_t target, curoff, ret;
    size_t n;
    struct stat st;
    int havepos;

    if (!(seekfn = fp->_seek)) {
        errno = ESPIPE;
        return -1;
    }

    /* Change any SEEK_CUR to SEEK_SET, and check "whence' argument. After this,
     * whence is either SEEK_SET or SEEK_END.
     */
    switch (whence) {
    case SEEK_CUR:
        if (_ftello(fp, &curoff))
            return -1;
        if (curoff < 0) {
            errno = ESPIPE;
            return -1;
        }
        if (offset > 0 && curoff > OFF_MAX - offset) {
            errno = EOVERFLOW;
            return -1;
        }
        offset += curoff;
        if (offset < 0) {
            errno = EINVAL;
            return -1;
        }
        if (ltest && offset > LONG_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
        whence = SEEK_SET;
        havepos = 1;
        break;

    case SEEK_SET:
        if (offset < 0) {
            errno = EINVAL;
            return -1;
        }
        /* FALLTHROUGH */
    case SEEK_END:
        curoff = 0;
        havepos = 0;
        break;

    default:
        errno = EINVAL;
        return -1;
    }

    /* Can only optimize if: reading (and not reading-and-writing); not
     * unbuffered; and this is a "regular' Unix file (and hence
     * seekfn==__sseek). We must check __SNBF first, because it is possible to
     * have __SNBF and __SOPT both set.
     */
    if (!fp->_bf._base)
        __smakebuf(fp);
    if (fp->_flags & (__SWR | __SRW | __SNBF | __SNPT))
        goto dumb;
    if ((fp->_flags & __SOPT) == 0) {
        if (seekfn != __sseek || fp->_file < 0 || fstat(fp->_file, &st) ||
            (st.st_mode & S_IFMT) != S_IFREG) {
            fp->_flags |= __SNPT;
            goto dumb;
        }
        fp->_blksize = (int) st.st_blksize;
        fp->_flags |= __SOPT;
    }

    /* We are reading; we can try to optimize. Figure out where we are going and
     * where we are now.
     */
    if (whence == SEEK_SET)
        target = offset;
    else {
        if (fstat(fp->_file, &st))
            goto dumb;
        if (offset > 0 && st.st_size > OFF_MAX - offset) {
            errno = EOVERFLOW;
            return -1;
        }
        target = st.st_size + offset;
        if ((off_t) target < 0) {
            errno = EINVAL;
            return -1;
        }
        if (ltest && (off_t) target > LONG_MAX) {
            errno = EOVERFLOW;
            return -1;
        }
    }

    if (!havepos && _ftello(fp, &curoff))
        goto dumb;

    /* Compute the number of bytes in the input buffer (pretending that any
     * ungetc() input has been discarded). Adjust current offset backwards by
     * this count so that it represents the file offset for the first byte in
     * the current input buffer.
     */
    if (HASUB(fp)) {
        curoff += fp->_r;
        n = (size_t) (fp->_up - fp->_bf._base);
        curoff -= (fpos_t) n;
        n += (size_t) fp->_ur;
    } else {
        n = (size_t) (fp->_p - fp->_bf._base);
        curoff -= (fpos_t) n;
        n += (size_t) fp->_r;
    }

    /* If the target offset is within the current buffer, simply adjust the
     * pointers, clear EOF, undo ungetc(), and return.
     */
    if (target >= curoff && target < curoff + (fpos_t) n) {
        size_t o = (size_t) (target - curoff);

        fp->_p = fp->_bf._base + o;
        fp->_r = (int) (n - o);
        if (HASUB(fp))
            FREEUB(fp);
        fp->_flags &= (unsigned short) ~__SEOF;
        return 0;
    }

    /* The place we want to get to is not within the current buffer, but we can
     * still be kind to the kernel copyout mechanism. By aligning the file
     * offset to a block boundary, we can let the kernel use the VM hardware to
     * map pages instead of copying bytes laboriously.
     */
    curoff = target & ~(fp->_blksize - 1);
    if (_sseek(fp, curoff, SEEK_SET) == POS_ERR)
        goto dumb;
    fp->_r = 0;
    fp->_p = fp->_bf._base;
    if (HASUB(fp))
        FREEUB(fp);
    n = (size_t) (target - curoff);
    if (n) {
        if (__srefill(fp) || (size_t) fp->_r < n)
            goto dumb;
        fp->_p += n;
        fp->_r -= (int) n;
    }
    fp->_flags &= (unsigned short) ~__SEOF;
    return 0;

    /* We get here if we cannot optimize the seek ... just do it. Allow the seek
     * function to change fp->_bf._base.
     */
dumb:
    if (__sflush(fp) || (ret = _sseek(fp, (fpos_t) offset, whence)) == POS_ERR)
        return -1;
    if (ltest && ret > LONG_MAX) {
        fp->_flags |= __SERR;
        errno = EOVERFLOW;
        return -1;
    }
    /* success: clear EOF indicator and discard ungetc() data */
    if (HASUB(fp))
        FREEUB(fp);
    fp->_p = fp->_bf._base;
    fp->_r = 0;
    fp->_flags &= (unsigned short) ~__SEOF;
    return 0;
}

int fseek(FILE *fp, long offset, int whence)
{
    int ret;
    int serrno = errno;

    if (!__sdidinit)
        __sinit();

    FLOCKFILE(fp);
    ret = _fseeko(fp, (off_t) offset, whence, 1);
    FUNLOCKFILE(fp);
    if (ret == 0)
        errno = serrno;
    return ret;
}

int fseeko(FILE *fp, off_t offset, int whence)
{
    int ret;
    int serrno = errno;

    if (!__sdidinit)
        __sinit();

    FLOCKFILE(fp);
    ret = _fseeko(fp, offset, whence, 0);
    FUNLOCKFILE(fp);
    if (ret == 0)
        errno = serrno;
    return ret;
}
