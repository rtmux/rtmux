/*
 * localtime / gmtime — convert time_t to broken-down time (UTC only)
 *
 * No timezone support — returns UTC always.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <time.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#define SECS_PER_MIN 60
#define SECS_PER_HOUR 3600
#define SECS_PER_DAY 86400

static const int mdays[2][12] = {
    {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
    {31, 29, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31},
};

static int is_leap(int y)
{
    return (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
}

static int days_in_year(int y)
{
    return is_leap(y) ? 366 : 365;
}

static struct tm result;

struct tm *gmtime_r(const time_t *tp, struct tm *tm)
{
    time_t t = *tp;
    int year = 1970;
    int days, leap;

    /* Handle negative (pre-epoch) crudely */
    if (t < 0)
        t = 0;

    days = (int) (t / SECS_PER_DAY);
    t %= SECS_PER_DAY;

    tm->tm_hour = (int) (t / SECS_PER_HOUR);
    t %= SECS_PER_HOUR;
    tm->tm_min = (int) (t / SECS_PER_MIN);
    tm->tm_sec = (int) (t % SECS_PER_MIN);

    tm->tm_wday = (days + 4) % 7; /* Jan 1 1970 = Thursday */

    while (days >= days_in_year(year)) {
        days -= days_in_year(year);
        year++;
    }
    tm->tm_year = year - 1900;
    tm->tm_yday = days;

    leap = is_leap(year);
    for (tm->tm_mon = 0; tm->tm_mon < 12; tm->tm_mon++) {
        if (days < mdays[leap][tm->tm_mon])
            break;
        days -= mdays[leap][tm->tm_mon];
    }
    tm->tm_mday = days + 1;
    tm->tm_isdst = 0;

    return tm;
}

struct tm *gmtime(const time_t *tp)
{
    return gmtime_r(tp, &result);
}

struct tm *localtime_r(const time_t *tp, struct tm *tm)
{
    return gmtime_r(tp, tm); /* No timezone — UTC always */
}

struct tm *localtime(const time_t *tp)
{
    return gmtime_r(tp, &result);
}

static const char *const asc_wday[7] = {"Sun", "Mon", "Tue", "Wed",
                                        "Thu", "Fri", "Sat"};
static const char *const asc_mon[12] = {"Jan", "Feb", "Mar", "Apr",
                                        "May", "Jun", "Jul", "Aug",
                                        "Sep", "Oct", "Nov", "Dec"};

char *asctime_r(const struct tm *tm, char *buf)
{
    char tmp[64];
    int n;

    if (!tm || !buf || tm->tm_wday < 0 || tm->tm_wday > 6 || tm->tm_mon < 0 ||
        tm->tm_mon > 11) {
        errno = EINVAL;
        return NULL;
    }

    /* POSIX "Www Mmm dd hh:mm:ss yyyy\n". Format into scratch first so an
     * out-of-range field cannot overrun the caller's required 26-byte buffer.
     * The year is widened before the +1900 so a near-INT_MAX tm_year cannot
     * signed-overflow before the length guard rejects it.
     */
    n = snprintf(tmp, sizeof tmp, "%s %s%3d %.2d:%.2d:%.2d %ld\n",
                 asc_wday[tm->tm_wday], asc_mon[tm->tm_mon], tm->tm_mday,
                 tm->tm_hour, tm->tm_min, tm->tm_sec,
                 (long) tm->tm_year + 1900L);
    if (n < 0 || n >= 26) {
        errno = EOVERFLOW;
        return NULL;
    }
    memcpy(buf, tmp, (size_t) n + 1);
    return buf;
}

char *ctime_r(const time_t *tp, char *buf)
{
    struct tm tm;

    if (!tp || !buf) {
        errno = EINVAL;
        return NULL;
    }
    return asctime_r(gmtime_r(tp, &tm), buf);
}

static char asc_result[26];

char *asctime(const struct tm *tm)
{
    return asctime_r(tm, asc_result);
}

char *ctime(const time_t *tp)
{
    return ctime_r(tp, asc_result);
}
