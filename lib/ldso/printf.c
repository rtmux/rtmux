/*
 * Copyright (c) The FreeBSD Project. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <inttypes.h>
#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include "printf.h"
#include "libc.h"

#define MAXNBUF (sizeof(intmax_t) * CHAR_BIT + 1)

#define PRINT_METHOD_SNPRINTF 1
#define PRINT_METHOD_WRITE 2

struct snprintf_arg {
    int method;
    char *str;
    char *buf;
    size_t remain;
    size_t buf_total;
    int fd;
};

static void printf_out(struct snprintf_arg *info)
{
    if (info->remain == info->buf_total)
        return;
    write(info->fd, info->buf, info->buf_total - info->remain);
    info->str = info->buf;
    info->remain = info->buf_total;
}

static void snprintf_func(int ch, struct snprintf_arg *const info)
{
    switch (info->method) {
    case PRINT_METHOD_SNPRINTF:
        if (info->remain >= 2) {
            *info->str++ = (char) ch;
            info->remain--;
        }
        break;
    case PRINT_METHOD_WRITE:
        if (info->remain == 0)
            printf_out(info);
        *info->str++ = (char) ch;
        info->remain--;
        break;
    }
}

static char const hex2ascii_lower[] = "0123456789abcdefghijklmnopqrstuvwxyz";
static char const hex2ascii_upper[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
#define hex2ascii(hex) (hex2ascii_lower[hex])
#define hex2ascii_upper(hex) (hex2ascii_upper[hex])

static __inline int imax(int a, int b)
{
    return (a > b ? a : b);
}

static char *ksprintn(char *nbuf,
                      uintmax_t num,
                      unsigned base,
                      int *lenp,
                      int upper)
{
    char *p, c;

    p = nbuf;
    *p = '\0';
    do {
        c = upper ? hex2ascii_upper(num % base) : hex2ascii(num % base);
        *++p = c;
    } while (num /= base);
    if (lenp)
        *lenp = (int) (p - nbuf);
    return (p);
}

static int kvprintf(char const *fmt,
                    struct snprintf_arg *arg,
                    int radix,
                    va_list ap)
{
#define PCHAR(c) snprintf_func((c), arg)
    char nbuf[MAXNBUF];
    const char *p, *percent, *q;
    u_char *up;
    int ch, n;
    uintmax_t num;
    unsigned base;
    int lflag, qflag, tmp, width, ladjust, sharpflag, neg, sign, dot;
    int cflag, hflag, jflag, tflag, zflag;
    int dwidth, upper;
    char padc;
    int stop = 0, retval = 0;

    num = 0;

    if (!fmt)
        fmt = "(fmt null)\n";

    if (radix < 2 || radix > 36)
        radix = 10;

    for (;;) {
        padc = ' ';
        width = 0;
        while ((ch = (u_char) *fmt++) != '%' || stop) {
            if (ch == '\0')
                return (retval);
            PCHAR(ch);
        }
        percent = fmt - 1;
        qflag = 0;
        lflag = 0;
        ladjust = 0;
        sharpflag = 0;
        neg = 0;
        sign = 0;
        dot = 0;
        dwidth = 0;
        upper = 0;
        cflag = 0;
        hflag = 0;
        jflag = 0;
        tflag = 0;
        zflag = 0;
    reswitch:
        switch (ch = (u_char) *fmt++) {
        case '.':
            dot = 1;
            goto reswitch;
        case '#':
            sharpflag = 1;
            goto reswitch;
        case '+':
            sign = 1;
            goto reswitch;
        case '-':
            ladjust = 1;
            goto reswitch;
        case '%':
            PCHAR(ch);
            break;
        case '*':
            if (!dot) {
                width = va_arg(ap, int);
                if (width < 0) {
                    ladjust = !ladjust;
                    width = -width;
                }
            } else {
                dwidth = va_arg(ap, int);
            }
            goto reswitch;
        case '0':
            if (!dot) {
                padc = '0';
                goto reswitch;
            }
            /* FALLTHROUGH */
        case '1':
        case '2':
        case '3':
        case '4':
        case '5':
        case '6':
        case '7':
        case '8':
        case '9':
            for (n = 0;; ++fmt) {
                n = n * 10 + ch - '0';
                ch = *fmt;
                if (ch < '0' || ch > '9')
                    break;
            }
            if (dot)
                dwidth = n;
            else
                width = n;
            goto reswitch;
        case 'b':
            num = (u_int) va_arg(ap, int);
            p = va_arg(ap, char *);
            for (q = ksprintn(nbuf, num, (unsigned) *p++, NULL, 0); *q;)
                PCHAR(*q--);

            if (num == 0)
                break;

            for (tmp = 0; *p;) {
                n = *p++;
                if (num & (1 << (n - 1))) {
                    PCHAR(tmp ? ',' : '<');
                    for (; (n = *p) > ' '; ++p)
                        PCHAR(n);
                    tmp = 1;
                } else
                    for (; *p > ' '; ++p)
                        continue;
            }
            if (tmp)
                PCHAR('>');
            break;
        case 'c':
            PCHAR(va_arg(ap, int));
            break;
        case 'D':
            up = va_arg(ap, u_char *);
            p = va_arg(ap, char *);
            if (!width)
                width = 16;
            while (width--) {
                PCHAR(hex2ascii(*up >> 4));
                PCHAR(hex2ascii(*up & 0x0f));
                up++;
                if (width)
                    for (q = p; *q; q++)
                        PCHAR(*q);
            }
            break;
        case 'd':
        case 'i':
            base = 10;
            sign = 1;
            goto handle_sign;
        case 'h':
            if (hflag) {
                hflag = 0;
                cflag = 1;
            } else
                hflag = 1;
            goto reswitch;
        case 'j':
            jflag = 1;
            goto reswitch;
        case 'l':
            if (lflag) {
                lflag = 0;
                qflag = 1;
            } else
                lflag = 1;
            goto reswitch;
        case 'n':
            if (jflag)
                *(va_arg(ap, intmax_t *)) = retval;
            else if (qflag)
                *(va_arg(ap, quad_t *)) = retval;
            else if (lflag)
                *(va_arg(ap, long *)) = retval;
            else if (zflag)
                *(va_arg(ap, size_t *)) = (size_t) retval;
            else if (hflag)
                *(va_arg(ap, short *)) = (short) retval;
            else if (cflag)
                *(va_arg(ap, char *)) = (char) retval;
            else
                *(va_arg(ap, int *)) = retval;
            break;
        case 'o':
            base = 8;
            goto handle_nosign;
        case 'p':
            base = 16;
            sharpflag = (width == 0);
            sign = 0;
            num = (uintptr_t) va_arg(ap, void *);
            goto number;
        case 'q':
            qflag = 1;
            goto reswitch;
        case 'r':
            base = (unsigned) radix;
            if (sign)
                goto handle_sign;
            goto handle_nosign;
        case 's':
            p = va_arg(ap, char *);
            if (!p)
                p = "(null)";
            if (!dot)
                n = (int) strlen(p);
            else
                for (n = 0; n < dwidth && p[n]; n++)
                    continue;

            width -= n;

            if (!ladjust && width > 0)
                while (width--)
                    PCHAR(padc);
            while (n--)
                PCHAR(*p++);
            if (ladjust && width > 0)
                while (width--)
                    PCHAR(padc);
            break;
        case 't':
            tflag = 1;
            goto reswitch;
        case 'u':
            base = 10;
            goto handle_nosign;
        case 'X':
            upper = 1;
            /* FALLTHROUGH */
        case 'x':
            base = 16;
            goto handle_nosign;
        case 'y':
            base = 16;
            sign = 1;
            goto handle_sign;
        case 'z':
            zflag = 1;
            goto reswitch;
        handle_nosign:
            sign = 0;
            if (jflag)
                num = va_arg(ap, uintmax_t);
            else if (qflag)
                num = va_arg(ap, u_quad_t);
            else if (tflag)
                num = (uintmax_t) va_arg(ap, ptrdiff_t);
            else if (lflag)
                num = va_arg(ap, u_long);
            else if (zflag)
                num = va_arg(ap, size_t);
            else if (hflag)
                num = (u_short) va_arg(ap, int);
            else if (cflag)
                num = (u_char) va_arg(ap, int);
            else
                num = va_arg(ap, u_int);
            goto number;
        handle_sign:
            if (jflag)
                num = (uintmax_t) va_arg(ap, intmax_t);
            else if (qflag)
                num = (uintmax_t) va_arg(ap, quad_t);
            else if (tflag)
                num = (uintmax_t) va_arg(ap, ptrdiff_t);
            else if (lflag)
                num = (uintmax_t) va_arg(ap, long);
            else if (zflag)
                num = (uintmax_t) va_arg(ap, ssize_t);
            else if (hflag)
                num = (uintmax_t) (short) va_arg(ap, int);
            else if (cflag)
                num = (uintmax_t) (char) va_arg(ap, int);
            else
                num = (uintmax_t) va_arg(ap, int);
        number:
            if (sign && (intmax_t) num < 0) {
                neg = 1;
                num = (uintmax_t) (-(intmax_t) num);
            }
            p = ksprintn(nbuf, num, base, &n, upper);
            tmp = 0;
            if (sharpflag && num != 0) {
                if (base == 8)
                    tmp++;
                else if (base == 16)
                    tmp += 2;
            }
            if (neg)
                tmp++;

            if (!ladjust && padc == '0')
                dwidth = width - tmp;
            width -= tmp + imax(dwidth, n);
            dwidth -= n;
            if (!ladjust)
                while (width-- > 0)
                    PCHAR(' ');
            if (neg)
                PCHAR('-');
            if (sharpflag && num != 0) {
                if (base == 8) {
                    PCHAR('0');
                } else if (base == 16) {
                    PCHAR('0');
                    PCHAR('x');
                }
            }
            while (dwidth-- > 0)
                PCHAR('0');

            while (*p)
                PCHAR(*p--);

            if (ladjust)
                while (width-- > 0)
                    PCHAR(' ');

            break;
        default:
            while (percent < fmt)
                PCHAR(*percent++);

            /* Since we ignore an formatting argument it is no longer safe to
             * obey the remaining formatting arguments as the arguments will no
             * longer match the format specs.
             */
            stop = 1;
            break;
        }
    }
#undef PCHAR
}

int ldso_snprintf(char *buf, size_t bufsize, const char *fmt, ...)
{
    va_list ap;
    int retval;

    va_start(ap, fmt);
    retval = ldso_vsnprintf(buf, bufsize, fmt, ap);
    va_end(ap);
    return (retval);
}

int ldso_vsnprintf(char *buf, size_t bufsize, const char *fmt, va_list ap)
{
    struct snprintf_arg info;
    int retval;

    info.method = PRINT_METHOD_SNPRINTF;
    info.buf = info.str = buf;
    info.buf_total = info.remain = bufsize;
    info.fd = -1;
    retval = kvprintf(fmt, &info, 10, ap);
    if (info.remain >= 1)
        *info.str++ = '\0';
    return (retval);
}

int ldso_vfdprintf(int fd, const char *fmt, va_list ap)
{
    char buf[512];
    struct snprintf_arg info;
    int retval;

    info.method = PRINT_METHOD_WRITE;
    info.buf = info.str = buf;
    info.buf_total = info.remain = sizeof(buf);
    info.fd = fd;
    retval = kvprintf(fmt, &info, 10, ap);
    printf_out(&info);
    return (retval);
}

int ldso_fdprintf(int fd, const char *fmt, ...)
{
    va_list ap;
    int retval;

    va_start(ap, fmt);
    retval = ldso_vfdprintf(fd, fmt, ap);
    va_end(ap);
    return (retval);
}

int ldso_fdprintfx(int fd, const char *fmt, ...)
{
    va_list ap;
    int retval;

    va_start(ap, fmt);
    retval = ldso_vfdprintf(fd, fmt, ap);
    va_end(ap);
    return (retval);
}

void ldso_fdputstr(int fd, const char *str)
{
    write(fd, str, strlen(str));
}

void ldso_fdputchar(int fd, int c)
{
    char c1;

    c1 = (char) c;
    write(fd, &c1, 1);
}
