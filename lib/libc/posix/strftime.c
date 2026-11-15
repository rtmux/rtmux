/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0 AND MIT
 */

/*
 * strftime - format broken-down time, following musl's strftime.c, with the C
 * (POSIX) locale's strings built in.
 *
 * Every POSIX.1-2024 conversion is supported, with the E and O modifiers
 * (accepted and ignored, as the C locale has no alternative forms), the 0 and
 * + flags and a minimum field width on %C, %F, %G and %Y. The GNU '-' and '_'
 * padding flags are accepted on numeric conversions. An unknown conversion, a
 * '%' at the end of the format, or a result that does not fit in maxsize bytes
 * (the NUL included) returns 0; the array contents are then indeterminate, but
 * no byte at or past buf[maxsize] is written and none is written at all when
 * maxsize is 0. As POSIX requires, every call acts as if it called tzset().
 *
 * Where POSIX leaves a choice: %Y and %G are at least four digits, with no '+'
 * for years past 9999 (musl adds one); %F is %+4Y-%m-%d, as POSIX defines it;
 * %s subtracts tm_gmtoff, so it is exact for a struct tm from localtime or
 * gmtime whatever TZ is now; %z and %Z are empty when tm_isdst is negative.
 *
 * Copyright (c) 2005-2020 Rich Felker, et al.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <ctype.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "time-conv.h"

static size_t format(char *restrict s,
                     size_t n,
                     const char *restrict f,
                     const struct tm *restrict tm);

/* Scratch for one conversion; the longest is %c with a 10-digit year. */
#define CONV_MAX 100

static const char *const day_name[7] = {"Sunday",    "Monday",   "Tuesday",
                                        "Wednesday", "Thursday", "Friday",
                                        "Saturday"};
static const char *const mon_name[12] = {
    "January", "February", "March",     "April",   "May",      "June",
    "July",    "August",   "September", "October", "November", "December"};

static int is_leap(int y)
{
    /* Avoid overflow */
    if (y > INT_MAX - 1900)
        y -= 2000;
    y += 1900;
    return !(y % 4) && ((y % 100) || !(y % 400));
}

/* ISO 8601 week number, 1-53: weeks start on Monday and week 1 holds the year's
 * first Thursday.
 */
static int week_num(const struct tm *tm)
{
    /* Unsigned, as in musl: out-of-range fields wrap instead of overflowing. */
    unsigned wday = (unsigned) tm->tm_wday, yday = (unsigned) tm->tm_yday;
    int val = (int) ((yday + 7U - (wday + 6U) % 7) / 7);

    /* If 1 January is just 1-3 days past Monday, the previous week is also in
     * this year.
     */
    if ((wday + 371U - yday - 2) % 7 <= 2)
        val++;
    if (!val) {
        /* If 31 December of the previous year is a Thursday, or a Friday of a
         * leap year, then that year has 53 weeks.
         */
        int dec31 = (int) ((wday + 7U - yday - 1) % 7);
        val = 52;
        if (dec31 == 4 || (dec31 == 5 && is_leap(tm->tm_year % 400 - 1)))
            val++;
    } else if (val == 53) {
        /* If 1 January is not a Thursday, and not a Wednesday of a leap year,
         * then this year has only 52 weeks.
         */
        int jan1 = (int) ((wday + 371U - yday) % 7);
        if (jan1 != 4 && (jan1 != 3 || !is_leap(tm->tm_year)))
            val = 1;
    }
    return val;
}

/* Expand one conversion into a string and its length *l, or return NULL for an
 * unknown conversion. `pad` is the flag character, or 0.
 */
static const char *fmt_1(char (*s)[CONV_MAX],
                         size_t *l,
                         int f,
                         const struct tm *tm,
                         int pad)
{
    long long val;
    const char *fmt = "-";
    int width = 2, def_pad = '0';
    size_t n;

    switch (f) {
    case 'a':
        if ((unsigned) tm->tm_wday > 6U)
            goto string;
        *l = 3; /* the abbreviation is the first three letters */
        return day_name[tm->tm_wday];
    case 'A':
        if ((unsigned) tm->tm_wday > 6U)
            goto string;
        fmt = day_name[tm->tm_wday];
        goto string;
    case 'h':
    case 'b':
        if ((unsigned) tm->tm_mon > 11U)
            goto string;
        *l = 3;
        return mon_name[tm->tm_mon];
    case 'B':
        if ((unsigned) tm->tm_mon > 11U)
            goto string;
        fmt = mon_name[tm->tm_mon];
        goto string;
    case 'c':
        fmt = "%a %b %e %H:%M:%S %Y";
        goto recu_strftime;
    case 'C':
        val = (1900LL + tm->tm_year) / 100;
        goto number;
    case 'e':
        def_pad = '_';
        /* fall through */
    case 'd':
        val = tm->tm_mday;
        goto number;
    case 'D':
    case 'x': /* the C locale's date */
        fmt = "%m/%d/%y";
        goto recu_strftime;
    case 'F':
        fmt = "%+4Y-%m-%d"; /* as POSIX defines %F */
        goto recu_strftime;
    case 'g':
    case 'G':
        val = tm->tm_year + 1900LL;
        if (tm->tm_yday < 3 && week_num(tm) != 1)
            val--;
        else if (tm->tm_yday > 360 && week_num(tm) == 1)
            val++;
        if (f == 'g') {
            val %= 100;
            if (val < 0)
                val = -val;
        } else {
            width = 4;
        }
        goto number;
    case 'H':
        val = tm->tm_hour;
        goto number;
    case 'I':
        val = tm->tm_hour;
        if (!val)
            val = 12;
        else if (val > 12)
            val -= 12;
        goto number;
    case 'j':
        val = tm->tm_yday + 1LL;
        width = 3;
        goto number;
    case 'm':
        val = tm->tm_mon + 1LL;
        goto number;
    case 'M':
        val = tm->tm_min;
        goto number;
    case 'n':
        *l = 1;
        return "\n";
    case 'p':
        fmt = tm->tm_hour >= 12 ? "PM" : "AM";
        goto string;
    case 'r':
        fmt = "%I:%M:%S %p";
        goto recu_strftime;
    case 'R':
        fmt = "%H:%M";
        goto recu_strftime;
    case 's':
        /* Only a wild tm_gmtoff can overflow; that struct tm has no %s. */
        if (__builtin_sub_overflow(__tm_to_secs(tm), (long long) tm->tm_gmtoff,
                                   &val))
            return NULL;
        width = 1;
        goto number;
    case 'S':
        val = tm->tm_sec;
        goto number;
    case 't':
        *l = 1;
        return "\t";
    case 'T':
    case 'X': /* the C locale's time */
        fmt = "%H:%M:%S";
        goto recu_strftime;
    case 'u':
        val = tm->tm_wday ? tm->tm_wday : 7;
        width = 1;
        goto number;
    case 'U':
        val = ((unsigned) tm->tm_yday + 7U - (unsigned) tm->tm_wday) / 7;
        goto number;
    case 'W':
        val =
            ((unsigned) tm->tm_yday + 7U - ((unsigned) tm->tm_wday + 6U) % 7) /
            7;
        goto number;
    case 'V':
        val = week_num(tm);
        goto number;
    case 'w':
        val = tm->tm_wday;
        width = 1;
        goto number;
    case 'y':
        val = (tm->tm_year + 1900LL) % 100;
        if (val < 0)
            val = -val;
        goto number;
    case 'Y':
        val = tm->tm_year + 1900LL;
        width = 4;
        goto number;
    case 'z': {
        /* The sign comes from the offset itself, so -00:00:30 is -0000. */
        long off = tm->tm_gmtoff;
        unsigned long mag =
            off < 0 ? 0UL - (unsigned long) off : (unsigned long) off;

        if (tm->tm_isdst < 0) {
            *l = 0;
            return "";
        }
        *l = (size_t) snprintf(*s, sizeof *s, "%c%02lu%02lu",
                               off < 0 ? '-' : '+', mag / 3600 % 100,
                               mag % 3600 / 60);
        return *s;
    }
    case 'Z':
        if (tm->tm_isdst < 0 || !tm->tm_zone) {
            *l = 0;
            return "";
        }
        fmt = tm->tm_zone;
        goto string;
    case '%':
        *l = 1;
        return "%";
    default:
        return NULL;
    }
number:
    switch (pad ? pad : def_pad) {
    case '-':
        *l = (size_t) snprintf(*s, sizeof *s, "%lld", val);
        break;
    case '_':
        *l = (size_t) snprintf(*s, sizeof *s, "%*lld", width, val);
        break;
    default: /* '0' */
        *l = (size_t) snprintf(*s, sizeof *s, "%0*lld", width, val);
        break;
    }
    return *s;
string:
    *l = strlen(fmt);
    return fmt;
recu_strftime:
    n = format(*s, sizeof *s, fmt, tm);
    if (!n)
        return NULL;
    *l = n;
    return *s;
}

static size_t format(char *restrict s,
                     size_t n,
                     const char *restrict f,
                     const struct tm *restrict tm)
{
    size_t l, k;
    char buf[CONV_MAX];
    char *end;
    const char *p, *t;
    int pad, plus;
    unsigned long width;

    for (l = 0; l < n; f++) {
        if (!*f) {
            s[l] = '\0';
            return l;
        }
        if (*f != '%') {
            s[l++] = *f;
            continue;
        }
        f++;
        pad = 0;
        if (*f == '-' || *f == '_' || *f == '0')
            pad = *f++;
        if ((plus = (*f == '+')))
            f++;
        if (isdigit((unsigned char) *f)) {
            width = strtoul(f, &end, 10);
            p = end;
        } else {
            width = 0;
            p = f;
        }
        if (*p == 'C' || *p == 'F' || *p == 'G' || *p == 'Y') {
            if (!width && p != f)
                width = 1;
        } else {
            width = 0;
        }
        if (pad == '-')
            width = 0; /* GNU '-': no padding, whatever the width */
        f = p;
        if (*f == 'E' || *f == 'O')
            f++;

        /* A width is padded below, so the conversion itself is zero-padded: a
         * space-padded one would hide the sign and digits from the trim.
         */
        t = fmt_1(&buf, &k, *f, tm, width ? 0 : pad);
        if (!t)
            break;
        if (width) {
            size_t d;
            int sign = *t == '-' ? '-' : 0;

            /* Trim off any sign and leading zeros, then count the remaining
             * digits to decide whether the + flag adds a sign. A negative value
             * keeps its own sign (the ISO year of 0000-01-01 is -1).
             */
            if (*t == '+' || *t == '-') {
                t++;
                k--;
            }
            for (; *t == '0' && isdigit((unsigned char) t[1]); t++, k--)
                ;
            if (width < k)
                width = k;
            for (d = 0; isdigit((unsigned char) t[d]); d++)
                ;
            if (!sign && plus && d + (width - k) >= (*p == 'C' ? 3 : 5))
                sign = '+';
            if (sign)
                width--;
            /* GNU '_' pads with spaces ahead of the sign; zeros follow it. */
            for (; pad == '_' && width > k && l < n; width--)
                s[l++] = ' ';
            if (sign && l < n)
                s[l++] = (char) sign;
            for (; width > k && l < n; width--)
                s[l++] = '0';
        }
        if (k > n - l)
            k = n - l;
        memcpy(s + l, t, k);
        l += k;
    }
    if (n) {
        if (l == n)
            l = n - 1;
        s[l] = '\0';
    }
    return 0;
}

size_t strftime(char *restrict s,
                size_t n,
                const char *restrict f,
                const struct tm *restrict tm)
{
    /* POSIX: local time zone information is used as though tzset() were called,
     * whatever the format.
     */
    tzset();
    return format(s, n, f, tm);
}
