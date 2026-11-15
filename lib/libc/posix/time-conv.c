/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0 AND MIT
 */

/*
 * Constant-time conversion between seconds since the epoch and broken-down
 * time, following musl's __secs_to_tm, __tm_to_secs, __year_to_secs and
 * __month_to_secs.
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

#include <limits.h>
#include <stddef.h>

#include "time-conv.h"

/* 2000-03-01, the day after a 400-year cycle's leap day: counting from here
 * puts every leap day at the end of its year.
 */
#define LEAPOCH (946684800LL + 86400 * (31 + 29))

#define DAYS_PER_400Y (365 * 400 + 97)
#define DAYS_PER_100Y (365 * 100 + 24)
#define DAYS_PER_4Y (365 * 4 + 1)

long long __year_to_secs(long long year, int *is_leap)
{
    int cycles, centuries, leaps, rem, dummy;

    /* Fast path for 1902..2038, where every fourth year is a leap year. */
    if ((unsigned long long) year - 2ULL <= 136) {
        int y = (int) year;
        leaps = (y - 68) >> 2;
        if (!((y - 68) & 3)) {
            leaps--;
            if (is_leap)
                *is_leap = 1;
        } else if (is_leap) {
            *is_leap = 0;
        }
        return 31536000LL * (y - 70) + 86400LL * leaps;
    }

    if (!is_leap)
        is_leap = &dummy;
    cycles = (int) ((year - 100) / 400);
    rem = (int) ((year - 100) % 400);
    if (rem < 0) {
        cycles--;
        rem += 400;
    }
    if (!rem) {
        *is_leap = 1;
        centuries = 0;
        leaps = 0;
    } else {
        if (rem >= 200) {
            centuries = rem >= 300 ? 3 : 2;
        } else {
            centuries = rem >= 100 ? 1 : 0;
        }
        rem -= centuries * 100;
        if (!rem) {
            *is_leap = 0;
            leaps = 0;
        } else {
            leaps = rem / 4;
            rem %= 4;
            *is_leap = !rem;
        }
    }

    leaps += 97 * cycles + 24 * centuries - *is_leap;

    return (year - 100) * 31536000LL + leaps * 86400LL + 946684800 + 86400;
}

int __month_to_secs(int month, int is_leap)
{
    static const int secs_through_month[] = {
        0,           31 * 86400,  59 * 86400,  90 * 86400,
        120 * 86400, 151 * 86400, 181 * 86400, 212 * 86400,
        243 * 86400, 273 * 86400, 304 * 86400, 334 * 86400,
    };
    int t = secs_through_month[month];

    if (is_leap && month >= 2)
        t += 86400;
    return t;
}

long long __tm_to_secs(const struct tm *tm)
{
    int is_leap;
    long long year = tm->tm_year;
    int month = tm->tm_mon;
    long long t;

    if (month >= 12 || month < 0) {
        int adj = month / 12;
        month %= 12;
        if (month < 0) {
            adj--;
            month += 12;
        }
        year += adj;
    }
    t = __year_to_secs(year, &is_leap);
    t += __month_to_secs(month, is_leap);
    t += 86400LL * (tm->tm_mday - 1LL);
    t += 3600LL * tm->tm_hour;
    t += 60LL * tm->tm_min;
    t += tm->tm_sec;
    return t;
}

int __secs_to_tm(long long t, struct tm *tm)
{
    /* Month lengths from March, so February (the leap month) comes last. */
    static const char days_in_month[] = {31, 30, 31, 30, 31, 31,
                                         30, 31, 30, 31, 31, 29};
    long long days, secs, years;
    int remdays, remsecs, remyears;
    int qc_cycles, c_cycles, q_cycles;
    int months, wday, yday, leap;

    /* Reject times whose year would overflow int before any arithmetic. */
    if (!__secs_in_range(t))
        return -1;

    secs = t - LEAPOCH;
    days = secs / 86400;
    remsecs = (int) (secs % 86400);
    if (remsecs < 0) {
        remsecs += 86400;
        days--;
    }

    wday = (int) ((3 + days) % 7); /* 2000-03-01 was a Wednesday */
    if (wday < 0)
        wday += 7;

    qc_cycles = (int) (days / DAYS_PER_400Y);
    remdays = (int) (days % DAYS_PER_400Y);
    if (remdays < 0) {
        remdays += DAYS_PER_400Y;
        qc_cycles--;
    }

    c_cycles = remdays / DAYS_PER_100Y;
    if (c_cycles == 4)
        c_cycles--;
    remdays -= c_cycles * DAYS_PER_100Y;

    q_cycles = remdays / DAYS_PER_4Y;
    if (q_cycles == 25)
        q_cycles--;
    remdays -= q_cycles * DAYS_PER_4Y;

    remyears = remdays / 365;
    if (remyears == 4)
        remyears--;
    remdays -= remyears * 365;

    leap = !remyears && (q_cycles || !c_cycles);
    yday = remdays + 31 + 28 + leap;
    if (yday >= 365 + leap)
        yday -= 365 + leap;

    years = remyears + 4 * q_cycles + 100 * c_cycles + 400LL * qc_cycles;

    /* At most 12 steps: the month table is the bound, not the year. */
    for (months = 0; days_in_month[months] <= remdays; months++)
        remdays -= days_in_month[months];

    if (months >= 10) {
        months -= 12;
        years++;
    }

    if (years + 100 > INT_MAX || years + 100 < INT_MIN)
        return -1;

    tm->tm_year = (int) (years + 100);
    tm->tm_mon = months + 2;
    tm->tm_mday = remdays + 1;
    tm->tm_wday = wday;
    tm->tm_yday = yday;
    tm->tm_hour = remsecs / 3600;
    tm->tm_min = remsecs / 60 % 60;
    tm->tm_sec = remsecs % 60;
    return 0;
}
