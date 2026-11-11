/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 1990, 1993
 * The Regents of the University of California. All rights reserved.
 *
 * Adapted for RTMux: stripped wide-char, locale, float formatting. Float
 * support is stubbed with "[float]" output.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <ctype.h>
#include "local.h"
#include "fvwrite.h"

/* Max number of pad chars generated in one shot */
#define PADSIZE 16
static char blanks[PADSIZE] = {' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
                               ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '};
static char zeroes[PADSIZE] = {'0', '0', '0', '0', '0', '0', '0', '0',
                               '0', '0', '0', '0', '0', '0', '0', '0'};

/* Conversion buffer size: enough for largest 64-bit value in octal + sign */
#define BUF 68

#define PRINT(ptr, len)                  \
    do {                                 \
        iovp->iov_base = (void *) (ptr); \
        iovp->iov_len = (size_t) (len);  \
        uio.uio_resid += (size_t) (len); \
        iovp++;                          \
        if (++uio.uio_iovcnt >= NIOV) {  \
            if (__sfvwrite(fp, &uio))    \
                goto error;              \
            iovp = iov;                  \
            uio.uio_iovcnt = 0;          \
            uio.uio_resid = 0;           \
        }                                \
    } while (0)

/* A string sink (vsnprintf) that has run out of room discards further output;
 * the field total is tallied separately through ret, so once the sink is full
 * there is no need to keep emitting padding. Without this a huge field width
 * spins the pad loop below for billions of no-op iterations.
 */
#define SINK_FULL(fp) (((fp)->_flags & __SSTR) && (fp)->_w <= 0)

#define PAD(howmany, with)               \
    do {                                 \
        if ((n = (howmany)) > 0) {       \
            while (n > PADSIZE) {        \
                if (SINK_FULL(fp))       \
                    break;               \
                PRINT(with, PADSIZE);    \
                n -= PADSIZE;            \
            }                            \
            if (n > 0 && !SINK_FULL(fp)) \
                PRINT(with, n);          \
        }                                \
    } while (0)

#define FLUSH()                                    \
    do {                                           \
        if (uio.uio_resid && __sfvwrite(fp, &uio)) \
            goto error;                            \
        uio.uio_iovcnt = 0;                        \
        uio.uio_resid = 0;                         \
        iovp = iov;                                \
    } while (0)

/* Accumulate into the int character count, failing per POSIX with EOVERFLOW if
 * the running total would exceed what an int can hold rather than overflowing
 * it. Every addition to ret goes through here; amount is always non-negative.
 */
#define ADD_RET(amount)                 \
    do {                                \
        if ((amount) > INT_MAX - ret) { \
            errno = EOVERFLOW;          \
            goto error;                 \
        }                               \
        ret += (amount);                \
    } while (0)

/* Accumulate a decimal digit into acc, clamping at INT_MAX instead of
 * overflowing the int on a pathologically long width or precision.
 */
#define CLAMP_DIGIT(acc, ch)                \
    do {                                    \
        int digit = (ch) - '0';             \
        if ((acc) > (INT_MAX - digit) / 10) \
            (acc) = INT_MAX;                \
        else                                \
            (acc) = 10 * (acc) + digit;     \
    } while (0)

/* Flags used during conversion. */
#define ALT 0x001
#define LADJUST 0x004
#define LONGINT 0x010
#define LLONGINT 0x020
#define SHORTINT 0x040
#define ZEROPAD 0x080
#define PTRINT 0x200
#define SIZINT 0x400
#define CHARINT 0x800

#define NIOV 8

/* Convert an unsigned long to ASCII digits, written right-to-left.
 *
 * Parameters:
 *   val       : value to convert
 *   endp      : one past the last byte of the destination buffer
 *   base      : numeric base (8, 10, or 16)
 *   uppercase : non-zero to use A-F for base 16
 * Return value: pointer to the first written digit (within the buffer).
 */
static char *__ultoa(unsigned long val, char *endp, int base, int uppercase)
{
    const char *xdigs = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    char *cp = endp;

    switch (base) {
    case 10:
        if (val < 10) {
            *--cp = (char) ('0' + val);
            return cp;
        }
        do {
            *--cp = (char) ('0' + val % 10);
            val /= 10;
        } while (val != 0);
        break;
    case 8:
        do {
            *--cp = '0' + (val & 7);
            val >>= 3;
        } while (val);
        break;
    case 16:
        do {
            *--cp = xdigs[val & 15];
            val >>= 4;
        } while (val);
        break;
    }
    return cp;
}

/* Core formatted-output engine. Assumes the stream is already prepared for
 * writing. Float conversions consume their argument and emit a "[float]"
 * placeholder.
 *
 * Parameters:
 *   fp   : destination stream
 *   fmt0 : format string
 *   ap   : variadic argument list
 * Return value: number of characters output, or EOF on write error.
 */
int __vfprintf(FILE *fp, const char *fmt0, va_list ap)
{
    const char *fmt;
    int ch;
    int n;
    char *cp;
    int flags;
    int ret;
    int width;
    int prec;
    char sign;
    unsigned long ulval;
    int base;
    int dprec;
    int fieldsz;
    int realsz;
    int size;
    char ox[2];

    char buf[BUF];
    struct __siov iov[NIOV];
    struct __suio uio;
    struct __siov *iovp;

    static const char xdigs_lower[] = "0123456789abcdef";
    static const char xdigs_upper[] = "0123456789ABCDEF";
    const char *xdigs = xdigs_lower;

    ORIENT(fp, -1);

    ret = 0;
    uio.uio_iov = iovp = iov;
    uio.uio_iovcnt = 0;
    uio.uio_resid = 0;

    fmt = fmt0;
    for (;;) {
        const char *startp;
        for (startp = fmt; (ch = *fmt) != '\0' && ch != '%'; fmt++)
            ;
        if ((n = (int) (fmt - startp)) != 0) {
            PRINT(startp, n);
            ADD_RET(n);
        }
        if (ch == '\0')
            goto done;
        fmt++; /* skip '%' */

        flags = 0;
        width = 0;
        prec = -1;
        sign = '\0';
        ox[1] = '\0';

    rflag:
        ch = *fmt++;
    reswitch:
        switch (ch) {
        case ' ':
            if (!sign)
                sign = ' ';
            goto rflag;
        case '#':
            flags |= ALT;
            goto rflag;
        case '*':
            width = va_arg(ap, int);
            if (width < 0) {
                flags |= LADJUST;
                /* Negating INT_MIN is undefined; clamp it to INT_MAX. */
                width = width == INT_MIN ? INT_MAX : -width;
            }
            goto rflag;
        case '-':
            flags |= LADJUST;
            goto rflag;
        case '+':
            sign = '+';
            goto rflag;
        case '.':
            if ((ch = *fmt++) == '*') {
                prec = va_arg(ap, int);
                if (prec < 0)
                    prec = -1;
                goto rflag;
            }
            prec = 0;
            while (ch >= '0' && ch <= '9') {
                CLAMP_DIGIT(prec, ch);
                ch = *fmt++;
            }
            goto reswitch;
        case '0':
            flags |= ZEROPAD;
            goto rflag;
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            n = 0;
            do {
                CLAMP_DIGIT(n, ch);
                ch = *fmt++;
            } while (ch >= '0' && ch <= '9');
            width = n;
            goto reswitch;
        case 'h':
            if (*fmt == 'h') {
                fmt++;
                flags |= CHARINT;
            } else
                flags |= SHORTINT;
            goto rflag;
        case 'j':
            flags |= LLONGINT;
            goto rflag;
        case 'l':
            if (*fmt == 'l') {
                fmt++;
                flags |= LLONGINT;
            } else
                flags |= LONGINT;
            goto rflag;
        case 'q':
            flags |= LLONGINT;
            goto rflag;
        case 't':
            flags |= PTRINT;
            goto rflag;
        case 'z':
            flags |= SIZINT;
            goto rflag;

        case 'c':
            *(cp = buf) = (char) va_arg(ap, int);
            size = 1;
            sign = '\0';
            goto pforw;

        case 'd':
        case 'i': {
            long sval;
            if (flags & LLONGINT)
                sval = va_arg(ap, long long);
            else if (flags & LONGINT)
                sval = va_arg(ap, long);
            else if (flags & PTRINT)
                sval = (long) (ptrdiff_t) va_arg(ap, ptrdiff_t);
            else if (flags & SIZINT)
                sval = (long) va_arg(ap, ssize_t);
            else if (flags & SHORTINT)
                sval = (short) va_arg(ap, int);
            else if (flags & CHARINT)
                sval = (signed char) va_arg(ap, int);
            else
                sval = va_arg(ap, int);
            if (sval < 0) {
                ulval = -(unsigned long) sval;
                sign = '-';
            } else {
                ulval = (unsigned long) sval;
            }
            base = 10;
            goto nosign;
        }

        case 'e':
        case 'E':
        case 'f':
        case 'F':
        case 'g':
        case 'G':
        case 'a':
        case 'A':
            /* Float: consume argument, print placeholder */
            (void) va_arg(ap, double);
            cp = "[float]";
            size = 7;
            sign = '\0';
            goto pforw;

        case 'n':
            if (flags & LLONGINT)
                *va_arg(ap, long long *) = ret;
            else if (flags & LONGINT)
                *va_arg(ap, long *) = ret;
            else if (flags & SHORTINT)
                *va_arg(ap, short *) = (short) ret;
            else if (flags & CHARINT)
                *va_arg(ap, signed char *) = (signed char) ret;
            else
                *va_arg(ap, int *) = ret;
            continue;

        case 'o':
            if (flags & (LLONGINT | LONGINT | PTRINT | SIZINT))
                ulval = va_arg(ap, unsigned long);
            else
                ulval = va_arg(ap, unsigned int);
            if (flags & SHORTINT)
                ulval = (unsigned short) ulval;
            else if (flags & CHARINT)
                ulval = (unsigned char) ulval;
            base = 8;
            goto nosign;

        case 'p':
            ulval = (unsigned long) (uintptr_t) va_arg(ap, void *);
            base = 16;
            xdigs = xdigs_lower;
            ox[1] = 'x';
            flags |= ALT;
            if (prec < 0)
                prec = (int) (sizeof(void *) * 2);
            goto nosign;

        case 's':
            cp = va_arg(ap, char *);
            if (!cp)
                cp = "(null)";
            if (prec >= 0) {
                char *p = memchr(cp, 0, (size_t) prec);
                size = p ? (int) (p - cp) : prec;
            } else
                size = (int) strlen(cp);
            sign = '\0';
            goto pforw;

        case 'u':
            if (flags & (LLONGINT | LONGINT | PTRINT | SIZINT))
                ulval = va_arg(ap, unsigned long);
            else
                ulval = va_arg(ap, unsigned int);
            if (flags & SHORTINT)
                ulval = (unsigned short) ulval;
            else if (flags & CHARINT)
                ulval = (unsigned char) ulval;
            base = 10;
            goto nosign;

        case 'X':
            xdigs = xdigs_upper;
            /* FALLTHROUGH */
        case 'x':
            if (ch == 'x')
                xdigs = xdigs_lower;
            if (flags & (LLONGINT | LONGINT | PTRINT | SIZINT))
                ulval = va_arg(ap, unsigned long);
            else
                ulval = va_arg(ap, unsigned int);
            if (flags & SHORTINT)
                ulval = (unsigned short) ulval;
            else if (flags & CHARINT)
                ulval = (unsigned char) ulval;
            base = 16;
            if (flags & ALT && ulval != 0)
                ox[1] = (char) ch;
            goto nosign;

        case '%':
            PRINT("%", 1);
            ADD_RET(1);
            continue;

        default:
            /* Unknown conversion, print it literally */
            if (ch == '\0')
                goto done;
            buf[0] = '%';
            buf[1] = (char) ch;
            PRINT(buf, 2);
            ADD_RET(2);
            continue;
        }

    nosign:
        FLUSH();
        cp = __ultoa(ulval, buf + BUF, base, xdigs == xdigs_upper);
        size = (int) (buf + BUF - cp);

    pforw:
        /* Right-justify by default, unless LADJUST */
        dprec = prec > size ? prec : size;
        fieldsz = dprec;

        /* The sign and the 0x prefix widen the field; guard the int against a
         * precision that has already reached its ceiling.
         */
        if (sign) {
            if (fieldsz == INT_MAX) {
                errno = EOVERFLOW;
                goto error;
            }
            fieldsz++;
        }
        if (ox[1]) {
            if (fieldsz > INT_MAX - 2) {
                errno = EOVERFLOW;
                goto error;
            }
            fieldsz += 2;
        }
        realsz = width > fieldsz ? width : fieldsz;

        /* Leading padding (spaces or zeroes) */
        if (!(flags & (LADJUST | ZEROPAD)))
            PAD(realsz - fieldsz, blanks);
        /* Sign */
        if (sign) {
            PRINT(&sign, 1);
        }
        /* 0x/0X prefix */
        if (ox[1]) {
            ox[0] = '0';
            PRINT(ox, 2);
        }
        /* Zero padding for precision and ZEROPAD */
        if (flags & ZEROPAD)
            PAD(realsz - fieldsz, zeroes);
        PAD(dprec - size, zeroes);
        /* The actual number or string */
        PRINT(cp, size);
        /* Trailing padding (left-adjusted only) */
        if (flags & LADJUST)
            PAD(realsz - fieldsz, blanks);

        ADD_RET(realsz);

        /* Drain the iov vector before the next iteration starts. PRINT() above
         * stashes pointers to two stack-locals -- &sign and ox[] -- but the
         * next iteration's "flags=0; sign='\0'; ox[1]='\0';" reset clobbers
         * those bytes before __sfvwrite has read them, emitting stray NUL bytes
         * mid-string.
         */
        FLUSH();
    }

done:
    FLUSH();
    return ret;

error:
    return EOF;
}

/* Standard vfprintf: prepare the stream for writing, then format.
 *
 * Parameters:
 *   fp  : destination stream
 *   fmt : format string
 *   ap  : variadic argument list
 * Return value: number of characters output, or EOF on error.
 */
int vfprintf(FILE *__restrict fp, const char *__restrict fmt, va_list ap)
{
    int ret;

    FLOCKFILE(fp);
    if (prepwrite(fp) != 0) {
        FUNLOCKFILE(fp);
        return EOF;
    }
    ret = __vfprintf(fp, fmt, ap);
    FUNLOCKFILE(fp);
    return ret;
}
