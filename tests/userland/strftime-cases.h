/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * strftime-cases.h - fixed C-locale strftime vectors, shared by the guest
 * suite (tests/userland/strftime-test.c) and the host test
 * (tests/host/test-strftime.c).
 *
 * The expected strings are fixed, not taken from the host libc at run time:
 * where POSIX leaves a choice open (the padding of %Y and %C for years before
 * 1000, %s for a struct tm whose tm_gmtoff differs from the host zone) the
 * vectors pin RTMux's documented behaviour. They were cross-checked against
 * host glibc in the C locale where the two must agree.
 *
 * Pure C with no libc calls, so it compiles unchanged in both worlds.
 */

#ifndef RTMUX_STRFTIME_CASES_H
#define RTMUX_STRFTIME_CASES_H

#include <limits.h>
#include <stddef.h>
#include <time.h>

/* Every POSIX.1-2024 conversion, in one format. */
#define STRFTIME_ALL                                                                  \
    "%a|%A|%b|%B|%c|%C|%d|%D|%e|%F|%g|%G|%h|%H|%I|%j|%m|%M|%n|%p|%r|%R|%S|%t|%T|%u|" \
    "%U|%V|%w|%W|%x|%X|%y|%Y|%z|%Z|%%|%s"

/* sec, min, hour, mday, mon, year, wday, yday, isdst, gmtoff, zone */
#define STRFTIME_TM(s, mi, h, d, mo, y, wd, yd, dst, off, z)                           \
    {.tm_sec = (s), .tm_min = (mi), .tm_hour = (h), .tm_mday = (d), .tm_mon = (mo),    \
     .tm_year = (y), .tm_wday = (wd), .tm_yday = (yd), .tm_isdst = (dst),               \
     .tm_gmtoff = (off), .tm_zone = (z)}

struct strftime_vec {
    struct tm tm;
    const char *want; /* STRFTIME_ALL applied to tm */
};

/* Twenty fixed times: the epoch and its eve, leap days, a leap second, the
 * ISO 8601 week-year edges (2004-W53 running into 2005, 2008-W01 starting in
 * 2007, 2009-W53, 2011-W52 ending in 2012), midnight and noon for %I/%p, both
 * sides of the 2025 US transitions, non-hour offsets for %z, and years 1, 1900
 * and 9999.
 */
static const struct strftime_vec strftime_vecs[] = {
    {STRFTIME_TM(0, 0, 0, 1, 0, 70, 4, 0, 0, 0, "UTC"),
     "Thu|Thursday|Jan|January|Thu Jan  1 00:00:00 1970|19|01|01/01/70| 1|1970-01-01|70|1970|"
     "Jan|00|12|001|01|00|\n|AM|12:00:00 AM|00:00|00|\t|00:00:00|4|00|01|4|00|01/01/70|"
     "00:00:00|70|1970|+0000|UTC|%|0"},
    {STRFTIME_TM(56, 34, 12, 29, 1, 100, 2, 59, 0, 0, "UTC"),
     "Tue|Tuesday|Feb|February|Tue Feb 29 12:34:56 2000|20|29|02/29/00|29|2000-02-29|00|2000|"
     "Feb|12|12|060|02|34|\n|PM|12:34:56 PM|12:34|56|\t|12:34:56|2|09|09|2|09|02/29/00|"
     "12:34:56|00|2000|+0000|UTC|%|951827696"},
    {STRFTIME_TM(59, 59, 23, 31, 11, 104, 5, 365, 0, 0, "UTC"),
     "Fri|Friday|Dec|December|Fri Dec 31 23:59:59 2004|20|31|12/31/04|31|2004-12-31|04|2004|"
     "Dec|23|11|366|12|59|\n|PM|11:59:59 PM|23:59|59|\t|23:59:59|5|52|53|5|52|12/31/04|"
     "23:59:59|04|2004|+0000|UTC|%|1104537599"},
    {STRFTIME_TM(3, 5, 8, 1, 0, 105, 6, 0, 0, 0, "UTC"),
     "Sat|Saturday|Jan|January|Sat Jan  1 08:05:03 2005|20|01|01/01/05| 1|2005-01-01|04|2004|"
     "Jan|08|08|001|01|05|\n|AM|08:05:03 AM|08:05|03|\t|08:05:03|6|00|53|6|00|01/01/05|"
     "08:05:03|05|2005|+0000|UTC|%|1104566703"},
    {STRFTIME_TM(0, 0, 9, 2, 0, 105, 0, 1, 0, 0, "UTC"),
     "Sun|Sunday|Jan|January|Sun Jan  2 09:00:00 2005|20|02|01/02/05| 2|2005-01-02|04|2004|"
     "Jan|09|09|002|01|00|\n|AM|09:00:00 AM|09:00|00|\t|09:00:00|7|01|53|0|00|01/02/05|"
     "09:00:00|05|2005|+0000|UTC|%|1104656400"},
    {STRFTIME_TM(0, 0, 10, 3, 0, 105, 1, 2, 0, 0, "UTC"),
     "Mon|Monday|Jan|January|Mon Jan  3 10:00:00 2005|20|03|01/03/05| 3|2005-01-03|05|2005|"
     "Jan|10|10|003|01|00|\n|AM|10:00:00 AM|10:00|00|\t|10:00:00|1|01|01|1|01|01/03/05|"
     "10:00:00|05|2005|+0000|UTC|%|1104746400"},
    {STRFTIME_TM(11, 11, 11, 31, 11, 107, 1, 364, 0, 0, "UTC"),
     "Mon|Monday|Dec|December|Mon Dec 31 11:11:11 2007|20|31|12/31/07|31|2007-12-31|08|2008|"
     "Dec|11|11|365|12|11|\n|AM|11:11:11 AM|11:11|11|\t|11:11:11|1|52|01|1|53|12/31/07|"
     "11:11:11|07|2007|+0000|UTC|%|1199099471"},
    {STRFTIME_TM(1, 0, 13, 29, 11, 108, 1, 363, 0, 0, "UTC"),
     "Mon|Monday|Dec|December|Mon Dec 29 13:00:01 2008|20|29|12/29/08|29|2008-12-29|09|2009|"
     "Dec|13|01|364|12|00|\n|PM|01:00:01 PM|13:00|01|\t|13:00:01|1|52|01|1|52|12/29/08|"
     "13:00:01|08|2008|+0000|UTC|%|1230555601"},
    {STRFTIME_TM(3, 2, 14, 31, 11, 109, 4, 364, 0, 0, "UTC"),
     "Thu|Thursday|Dec|December|Thu Dec 31 14:02:03 2009|20|31|12/31/09|31|2009-12-31|09|2009|"
     "Dec|14|02|365|12|02|\n|PM|02:02:03 PM|14:02|03|\t|14:02:03|4|52|53|4|52|12/31/09|"
     "14:02:03|09|2009|+0000|UTC|%|1262268123"},
    {STRFTIME_TM(5, 4, 15, 3, 0, 110, 0, 2, 0, 0, "UTC"),
     "Sun|Sunday|Jan|January|Sun Jan  3 15:04:05 2010|20|03|01/03/10| 3|2010-01-03|09|2009|"
     "Jan|15|03|003|01|04|\n|PM|03:04:05 PM|15:04|05|\t|15:04:05|7|01|53|0|00|01/03/10|"
     "15:04:05|10|2010|+0000|UTC|%|1262531045"},
    {STRFTIME_TM(7, 6, 16, 1, 0, 112, 0, 0, 0, 0, "UTC"),
     "Sun|Sunday|Jan|January|Sun Jan  1 16:06:07 2012|20|01|01/01/12| 1|2012-01-01|11|2011|"
     "Jan|16|04|001|01|06|\n|PM|04:06:07 PM|16:06|07|\t|16:06:07|7|01|52|0|00|01/01/12|"
     "16:06:07|12|2012|+0000|UTC|%|1325433967"},
    {STRFTIME_TM(60, 59, 23, 31, 11, 99, 5, 364, 0, 0, "UTC"),
     "Fri|Friday|Dec|December|Fri Dec 31 23:59:60 1999|19|31|12/31/99|31|1999-12-31|99|1999|"
     "Dec|23|11|365|12|59|\n|PM|11:59:60 PM|23:59|60|\t|23:59:60|5|52|52|5|52|12/31/99|"
     "23:59:60|99|1999|+0000|UTC|%|946684800"},
    {STRFTIME_TM(0, 0, 3, 9, 2, 125, 0, 67, 1, -14400, "EDT"),
     "Sun|Sunday|Mar|March|Sun Mar  9 03:00:00 2025|20|09|03/09/25| 9|2025-03-09|25|2025|"
     "Mar|03|03|068|03|00|\n|AM|03:00:00 AM|03:00|00|\t|03:00:00|7|10|10|0|09|03/09/25|"
     "03:00:00|25|2025|-0400|EDT|%|1741503600"},
    {STRFTIME_TM(0, 30, 1, 2, 10, 125, 0, 305, 0, -18000, "EST"),
     "Sun|Sunday|Nov|November|Sun Nov  2 01:30:00 2025|20|02|11/02/25| 2|2025-11-02|25|2025|"
     "Nov|01|01|306|11|30|\n|AM|01:30:00 AM|01:30|00|\t|01:30:00|7|44|44|0|43|11/02/25|"
     "01:30:00|25|2025|-0500|EST|%|1762065000"},
    {STRFTIME_TM(59, 59, 23, 31, 11, 69, 3, 364, 0, 0, "UTC"),
     "Wed|Wednesday|Dec|December|Wed Dec 31 23:59:59 1969|19|31|12/31/69|31|1969-12-31|70|1970|"
     "Dec|23|11|365|12|59|\n|PM|11:59:59 PM|23:59|59|\t|23:59:59|3|52|01|3|52|12/31/69|"
     "23:59:59|69|1969|+0000|UTC|%|-1"},
    {STRFTIME_TM(0, 0, 0, 1, 0, 0, 1, 0, 0, 0, "UTC"),
     "Mon|Monday|Jan|January|Mon Jan  1 00:00:00 1900|19|01|01/01/00| 1|1900-01-01|00|1900|"
     "Jan|00|12|001|01|00|\n|AM|12:00:00 AM|00:00|00|\t|00:00:00|1|00|01|1|01|01/01/00|"
     "00:00:00|00|1900|+0000|UTC|%|-2208988800"},
    {STRFTIME_TM(59, 59, 23, 31, 11, 8099, 5, 364, 0, 0, "UTC"),
     "Fri|Friday|Dec|December|Fri Dec 31 23:59:59 9999|99|31|12/31/99|31|9999-12-31|99|9999|"
     "Dec|23|11|365|12|59|\n|PM|11:59:59 PM|23:59|59|\t|23:59:59|5|52|52|5|52|12/31/99|"
     "23:59:59|99|9999|+0000|UTC|%|253402300799"},
    {STRFTIME_TM(0, 0, 0, 1, 0, -1899, 1, 0, 0, 0, "UTC"),
     "Mon|Monday|Jan|January|Mon Jan  1 00:00:00 0001|00|01|01/01/01| 1|0001-01-01|01|0001|"
     "Jan|00|12|001|01|00|\n|AM|12:00:00 AM|00:00|00|\t|00:00:00|1|00|01|1|01|01/01/01|"
     "00:00:00|01|0001|+0000|UTC|%|-62135596800"},
    {STRFTIME_TM(9, 5, 13, 15, 5, 124, 6, 166, 0, 19800, "IST"),
     "Sat|Saturday|Jun|June|Sat Jun 15 13:05:09 2024|20|15|06/15/24|15|2024-06-15|24|2024|"
     "Jun|13|01|167|06|05|\n|PM|01:05:09 PM|13:05|09|\t|13:05:09|6|23|24|6|24|06/15/24|"
     "13:05:09|24|2024|+0530|IST|%|1718436909"},
    {STRFTIME_TM(0, 0, 12, 29, 1, 116, 1, 59, 0, -9000, "NDT"),
     "Mon|Monday|Feb|February|Mon Feb 29 12:00:00 2016|20|29|02/29/16|29|2016-02-29|16|2016|"
     "Feb|12|12|060|02|00|\n|PM|12:00:00 PM|12:00|00|\t|12:00:00|1|09|09|1|09|02/29/16|"
     "12:00:00|16|2016|-0230|NDT|%|1456756200"},
};

/* The index of 2025-11-02 01:30:00 EST above, the usual base for edges. */
#define STRFTIME_VEC_EST 13

/* One format applied to one of the vectors above (by index). */
struct strftime_fmt_vec {
    size_t tm_index;
    const char *fmt;
    const char *want;
};

/* Modifiers, flags and widths. E and O change nothing in the C locale; the 0
 * and + flags and a width apply to %C, %F, %G and %Y.
 */
static const struct strftime_fmt_vec strftime_fmt_vecs[] = {
    {STRFTIME_VEC_EST, "%Ec|%EC|%Ex|%EX|%Ey|%EY", "Sun Nov  2 01:30:00 2025|20|11/02/25|01:30:00|25|2025"},
    {STRFTIME_VEC_EST, "%Od|%Oe|%OH|%OI|%Om|%OM|%OS|%Ou|%OU|%OV|%Ow|%OW|%Oy",
     "02| 2|01|01|11|30|00|7|44|44|0|43|25"},
    {STRFTIME_VEC_EST, "%+4Y|%+5Y|%05Y|%6Y|%+3C|%4C|%+12F|%010F|%+6G|%5G",
     "2025|+2025|02025|002025|+20|0020|+02025-11-02|2025-11-02|+02025|02025"},
    {17, "%Y|%+4Y|%C|%G|%F|%+11F", "0001|0001|00|0001|0001-01-01|+0001-01-01"},
    {16, "%+5Y|%+4Y|%F", "+9999|9999|9999-12-31"},
    {STRFTIME_VEC_EST, "%-d|%_d|%0e|%-j|%_H|%-m|%_y", "2| 2|02|306| 1|11|25"},
    {STRFTIME_VEC_EST, "%_6Y|%-6Y|%_+6Y|%_4C", "  2025|2025| +2025|  20"},
    {17, "%_6Y|%-6Y|%_3C", "     1|1|  0"},
    {0, "", ""},
    {0, "plain text, no conversions", "plain text, no conversions"},
};

/* Formats on a struct tm of their own, for edges outside the vectors above. */
struct strftime_tm_vec {
    struct tm tm;
    const char *fmt;
    const char *want;
};

static const struct strftime_tm_vec strftime_tm_vecs[] = {
    /* A negative offset under a minute keeps its sign. */
    {STRFTIME_TM(0, 0, 0, 1, 0, 70, 4, 0, 0, -30, "LMT"), "%z", "-0000"},
    {STRFTIME_TM(0, 0, 0, 1, 0, 70, 4, 0, 0, 30, "LMT"), "%z", "+0000"},
    /* 0000-01-01 is a Saturday in ISO week 52 of year -1; 0000-01-03 starts
     * week 1 of year 0.
     */
    {STRFTIME_TM(0, 0, 0, 1, 0, -1900, 6, 0, 0, 0, "UTC"), "%Y|%C|%G|%04G|%V",
     "0000|00|-001|-001|52"},
    {STRFTIME_TM(0, 0, 0, 3, 0, -1900, 1, 2, 0, 0, "UTC"), "%G|%V|%F",
     "0000|01|0000-01-03"},
    /* A tm_gmtoff far outside any zone cannot give %s; strftime returns 0. */
    {STRFTIME_TM(0, 0, 0, 1, 0, 70, 4, 0, 0, LONG_MIN, "BAD"),
     "%s", ""},
};

#endif
