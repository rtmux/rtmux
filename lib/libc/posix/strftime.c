/*
 * strftime — format broken-down time (minimal subset)
 *
 * Supports: %Y %m %d %H %M %S %T %h %b %% and literals. No locale support.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <time.h>
#include <stdio.h>
#include <string.h>

static const char *month_abbr[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

static const char *wday_abbr[] = {"Sun", "Mon", "Tue", "Wed",
                                  "Thu", "Fri", "Sat"};

size_t strftime(char *buf, size_t maxsize, const char *fmt, const struct tm *tm)
{
    char *dst = buf;
    char *end = buf + maxsize;
    char tmp[16];

    while (*fmt && dst < end - 1) {
        if (*fmt != '%') {
            *dst++ = *fmt++;
            continue;
        }
        fmt++;
        tmp[0] = '\0';

        switch (*fmt) {
        case 'Y':
            snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900);
            break;
        case 'm':
            snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1);
            break;
        case 'd':
            snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday);
            break;
        case 'H':
            snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour);
            break;
        case 'M':
            snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min);
            break;
        case 'S':
            snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec);
            break;
        case 'T': /* %H:%M:%S */
            snprintf(tmp, sizeof(tmp), "%02d:%02d:%02d", tm->tm_hour,
                     tm->tm_min, tm->tm_sec);
            break;
        case 'h': /* same as %b */
        case 'b':
            if (tm->tm_mon >= 0 && tm->tm_mon < 12)
                snprintf(tmp, sizeof(tmp), "%s", month_abbr[tm->tm_mon]);
            break;
        case 'a':
            if (tm->tm_wday >= 0 && tm->tm_wday < 7)
                snprintf(tmp, sizeof(tmp), "%s", wday_abbr[tm->tm_wday]);
            break;
        case '%':
            tmp[0] = '%';
            tmp[1] = '\0';
            break;
        default:
            tmp[0] = '%';
            tmp[1] = *fmt;
            tmp[2] = '\0';
            break;
        }

        size_t len = strlen(tmp);
        if (dst + len >= end)
            break;
        memcpy(dst, tmp, len);
        dst += len;
        fmt++;
    }

    *dst = '\0';
    return (size_t) (dst - buf);
}
