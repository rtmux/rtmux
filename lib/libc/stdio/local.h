/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: removed threading, wchar, locale dependencies.
 */

#ifndef _STDIO_LOCAL_H
#define _STDIO_LOCAL_H

#include <sys/types.h>
#include <sys/cdefs.h>
#include <string.h>
#include <stdlib.h>

/* Information local to this implementation of stdio, in particular, macros and
 * private variables.
 */

extern int _sread(FILE *, char *, int) __hidden;
extern int _swrite(FILE *, char const *, int) __hidden;
extern fpos_t _sseek(FILE *, fpos_t, int) __hidden;
extern int __sflush(FILE *) __hidden;
extern int __fflush(FILE *fp) __hidden;
extern FILE *__sfp(void) __hidden;
extern int __srefill(FILE *) __hidden;
extern int __sread(void *, char *, int) __hidden;
extern int __swrite(void *, char const *, int) __hidden;
extern fpos_t __sseek(void *, fpos_t, int) __hidden;
extern int __sclose(void *) __hidden;
extern void __sinit(void) __hidden;
extern void _cleanup(void) __hidden;
extern void __smakebuf(FILE *) __hidden;
extern unsigned __swhatbuf(FILE *, size_t *, int *) __hidden;
extern int _fwalk(int (*)(FILE *)) __hidden;
extern int __swsetup(FILE *) __hidden;
extern int __sflags(const char *, int *) __hidden;
extern int __ungetc(int, FILE *) __hidden;
extern int __vfprintf(FILE *, const char *, va_list) __hidden;
extern int _ftello(FILE *, fpos_t *) __hidden;
extern int __srget(FILE *) __hidden;
extern int __swbuf(int, FILE *) __hidden;
extern size_t __fread(void *__restrict buf,
                      size_t size,
                      size_t count,
                      FILE *__restrict fp) __hidden;
extern int __sdidinit __hidden;

#define __SLBF 0x0001 /* line buffered */
#define __SNBF 0x0002 /* unbuffered */
#define __SRD 0x0004  /* OK to read */
#define __SWR 0x0008  /* OK to write */
/* RD and WR are never simultaneously asserted */
#define __SRW 0x0010  /* open for reading & writing */
#define __SEOF 0x0020 /* found EOF */
#define __SERR 0x0040 /* found error */
#define __SMBF 0x0080 /* _bf._base is from malloc */
#define __SAPP 0x0100 /* fdopen()ed in append mode */
#define __SSTR 0x0200 /* this is an sprintf/snprintf string */
#define __SOPT 0x0400 /* do fseek() optimization */
#define __SNPT 0x0800 /* do not do fseek() optimization */
#define __SOFF 0x1000 /* set iff _offset is in fact correct */
#define __SALC 0x4000 /* allocate string space dynamically */
#define __SIGN 0x8000 /* ignore this file in _fwalk */

/* Helper for clearing flags without sign-conversion warnings */
#define __sclear(fp, flags) \
    ((fp)->_flags &= (unsigned short) ~(unsigned) (flags))

#define __S2OAP 0x0001 /* O_APPEND mode is set */

/* Prepare the given FILE for writing, and return 0 iff it can be written now.
 * Otherwise, return EOF and set errno.
 */
#define prepwrite(fp)                                        \
    ((((fp)->_flags & __SWR) == 0 ||                         \
      (!(fp)->_bf._base && ((fp)->_flags & __SSTR) == 0)) && \
     __swsetup(fp))

/* Test whether the given stdio file has an active ungetc buffer; release such a
 * buffer, without restoring ordinary unread data.
 */
#define HASUB(fp) ((fp)->_ub._base)
#define FREEUB(fp)                          \
    {                                       \
        if ((fp)->_ub._base != (fp)->_ubuf) \
            free((char *) (fp)->_ub._base); \
        (fp)->_ub._base = NULL;             \
    }

/* Structure initializations for 'fake' FILE objects. */
#define FAKE_FILE    \
    {                \
        ._file = -1, \
    }

/* No-op orientation (no wide-char support) */
#define ORIENT(fp, o) ((void) 0)

/* Per-FILE recursive locking (see flockfile.c). */
void __flockfile(FILE *fp);
int __ftrylockfile(FILE *fp);
void __funlockfile(FILE *fp);
#define FLOCKFILE(fp) __flockfile(fp)
#define FUNLOCKFILE(fp) __funlockfile(fp)

#define __sgetc(p) (--(p)->_r < 0 ? __srget(p) : (int) (*(p)->_p++))
#define __sfeof(p) (((p)->_flags & __SEOF) != 0)
#define __sferror(p) (((p)->_flags & __SERR) != 0)
#define __sclearerr(p) ((void) (__sclear((p), __SERR | __SEOF)))
#define __sfileno(p) ((p)->_file)

static __inline int __sputc(int _c, FILE *_p)
{
    if (--_p->_w >= 0 || (_p->_w >= _p->_lbfsize && (char) _c != '\n'))
        return (*_p->_p++ = (unsigned char) _c);
    return __swbuf(_c, _p);
}

#endif /* _STDIO_LOCAL_H */
