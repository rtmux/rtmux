/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * time-conv.h - calendar arithmetic shared by the libc time functions.
 *
 * Every routine runs in constant time: none loops once per year, so the cost of
 * a conversion does not depend on the date. Years are counted from 1900, as in
 * tm_year.
 */
#ifndef LIBC_TIME_CONV_H
#define LIBC_TIME_CONV_H

#include <limits.h>
#include <time.h>

/* The longest year in seconds. */
#define SECS_PER_LONG_YEAR 31622400LL

/* Whether t lies within INT_MIN..INT_MAX longest years of the epoch, so its
 * year can be computed without overflow. The year may still miss tm_year by the
 * 1900 bias; __secs_to_tm checks that.
 */
static inline int __secs_in_range(long long t)
{
    return t >= INT_MIN * SECS_PER_LONG_YEAR &&
           t <= INT_MAX * SECS_PER_LONG_YEAR;
}

/* Seconds from the epoch to 1 January of `year`; sets *is_leap if non-NULL. */
long long __year_to_secs(long long year, int *is_leap);

/* Seconds from 1 January to the first of `month` (0-11). */
int __month_to_secs(int month, int is_leap);

/* Seconds since the epoch of the fields of *tm, read as UTC. Out-of-range
 * fields are folded in (tm_mon 13 is February of the next year); tm_wday,
 * tm_yday, tm_isdst and the zone fields are ignored. Cannot overflow for any
 * int field values.
 */
long long __tm_to_secs(const struct tm *tm);

/* Fill the calendar fields of *tm (all but tm_isdst and the zone fields) from
 * seconds since the epoch, read as UTC.
 *
 * Returns -1, leaving *tm unspecified, when the year does not fit tm_year.
 */
int __secs_to_tm(long long t, struct tm *tm);

#endif
