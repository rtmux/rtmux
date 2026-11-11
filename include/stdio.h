/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by Chris Torek.
 *
 * Adapted for RTMux.
 */

#ifndef _STDIO_H_
#define _STDIO_H_

#include <sys/cdefs.h>
#include <stddef.h>
#include <sys/_offsettypes.h>
#include <sys/_sizetypes.h>

#ifndef _VA_LIST_DECLARED
#include <stdarg.h>
#define _VA_LIST_DECLARED
#endif

#define _FSTDIO

/* stdio buffers */
struct __sbuf {
    unsigned char *_base;
    int _size;
};

/* stdio state variables.
 *
 * The following always hold:
 *
 * if _flags selects line-buffered writes,
 *      _lbfsize is -_bf._size, else _lbfsize is 0
 * if _flags selects reads, _w is 0; if it selects writes, _r is 0
 */
struct __sFILE {
    unsigned char *_p;     /* current position in (some) buffer */
    int _r;                /* read space left for getc() */
    int _w;                /* write space left for putc() */
    unsigned short _flags; /* implementation flags; this FILE is free if 0 */
    short _file;           /* fileno, if Unix descriptor, else -1 */
    struct __sbuf _bf;     /* the buffer (at least 1 byte, if !NULL) */
    int _lbfsize;          /* 0 or -_bf._size, for inline putc */

    /* operations */
    void *_cookie; /* cookie passed to io functions */
    int (*_close)(void *);
    int (*_read)(void *, char *, int);
    fpos_t (*_seek)(void *, fpos_t, int);
    int (*_write)(void *, const char *, int);

    /* separate buffer for long sequences of ungetc() */
    struct __sbuf _ub;  /* ungetc buffer */
    unsigned char *_up; /* saved _p when _p is doing ungetc data */
    int _ur;            /* saved _r when _r is counting ungetc data */

    /* tricks to meet minimum requirements even when malloc() fails */
    unsigned char _ubuf[3]; /* guarantee an ungetc() buffer */
    unsigned char _nbuf[1]; /* guarantee a getc() buffer */

    /* Unix stdio files get aligned to block boundaries on fseek() */
    int _blksize;   /* stat.st_blksize (may be != _bf._size) */
    fpos_t _offset; /* current lseek offset */

    /* Threading fields — reserved by the FILE layout, unused in RTMux */
    void *_fl_mutex;
    void *_fl_owner;
    int _fl_count;
    int _orientation;
    int _flags2;
};

#ifndef _STDFILE_DECLARED
#define _STDFILE_DECLARED
typedef struct __sFILE FILE;
#endif

#ifndef _STDSTREAM_DECLARED
__BEGIN_DECLS
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
__END_DECLS
#define _STDSTREAM_DECLARED
#endif

#define _IOFBF 0 /* setvbuf should set fully buffered */
#define _IOLBF 1 /* setvbuf should set line buffered */
#define _IONBF 2 /* setvbuf should set unbuffered */

#define BUFSIZ 1024 /* size of buffer used by setbuf */
#define EOF (-1)

#ifndef FOPEN_MAX
#define FOPEN_MAX 20 /* must be <= OPEN_MAX */
#endif
#define FILENAME_MAX 1024

#ifndef SEEK_SET
#define SEEK_SET 0
#endif
#ifndef SEEK_CUR
#define SEEK_CUR 1
#endif
#ifndef SEEK_END
#define SEEK_END 2
#endif

#define stdin stdin
#define stdout stdout
#define stderr stderr

__BEGIN_DECLS

/* ANSI C functions */
void clearerr(FILE *);
int fclose(FILE *);
int feof(FILE *);
int ferror(FILE *);
int fflush(FILE *);
int fgetc(FILE *);
char *fgets(char *__restrict, int, FILE *__restrict);
FILE *fopen(const char *__restrict, const char *__restrict);
int fprintf(FILE *__restrict, const char *__restrict, ...) __printflike(2, 3);
int fputc(int, FILE *);
int fputs(const char *__restrict, FILE *__restrict);
size_t fread(void *__restrict, size_t, size_t, FILE *__restrict);
int fseek(FILE *, long, int);
long ftell(FILE *);
size_t fwrite(const void *__restrict, size_t, size_t, FILE *__restrict);
int getc(FILE *);
int getchar(void);
void perror(const char *);
int printf(const char *__restrict, ...) __printflike(1, 2);
int putc(int, FILE *);
int putchar(int);
int puts(const char *);
void rewind(FILE *);
void setbuf(FILE *__restrict, char *__restrict);
int setvbuf(FILE *__restrict, char *__restrict, int, size_t);
int sprintf(char *__restrict, const char *__restrict, ...) __printflike(2, 3);
int ungetc(int, FILE *);
int vfprintf(FILE *__restrict, const char *__restrict, va_list)
    __printflike(2, 0);
int vprintf(const char *__restrict, va_list) __printflike(1, 0);
int vsprintf(char *__restrict, const char *__restrict, va_list)
    __printflike(2, 0);
int snprintf(char *__restrict, size_t, const char *__restrict, ...)
    __printflike(3, 4);
int vsnprintf(char *__restrict, size_t, const char *__restrict, va_list)
    __printflike(3, 0);

/* POSIX */
FILE *fdopen(int, const char *);
int fileno(FILE *);
int fseeko(FILE *, off_t, int);
off_t ftello(FILE *);

/* POSIX thread-safe stdio locking (_POSIX_THREAD_SAFE_FUNCTIONS). flockfile()
 * is recursive: a thread already holding the FILE may re-lock it. The _unlocked
 * accessors skip the lock, for use inside a flockfile() region.
 */
void flockfile(FILE *);
int ftrylockfile(FILE *);
void funlockfile(FILE *);
int getc_unlocked(FILE *);
int getchar_unlocked(void);
int putc_unlocked(int, FILE *);
int putchar_unlocked(int);

__END_DECLS

#endif /* !_STDIO_H_ */
