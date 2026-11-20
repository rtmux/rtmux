/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * time_test - gmtime, localtime, mktime, timegm and the POSIX TZ parser.
 *
 * Covers the pre-epoch and overflow edges of gmtime, the cost bound of the
 * conversion (no loop over years), timegm round trips over representable times,
 * and mktime across both DST transitions of TZ=EST5EDT,M3.2.0,M11.1.0,
 * including the spring-forward gap and both sides of the fall-back fold.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "test.h"

#define US_TZ "EST5EDT,M3.2.0,M11.1.0"

/* 2025 transitions of US_TZ, in UTC. */
#define SPRING_2025 \
    1741503600LL /* 2025-03-09 07:00:00 UTC, 02:00 EST -> 03:00 EDT */
#define FALL_2025 \
    1762063200LL /* 2025-11-02 06:00:00 UTC, 02:00 EDT -> 01:00 EST */

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void set_tz(const char *tz)
{
    if (tz)
        setenv("TZ", tz, 1);
    else
        unsetenv("TZ");
    tzset();
}

/* Compare the calendar fields of *tm against a "YYYY-MM-DD hh:mm:ss" string. */
static void expect_tm(const char *name,
                      const struct tm *tm,
                      const char *want,
                      int isdst,
                      long gmtoff,
                      const char *zone)
{
    char got[96], detail[192];

    if (!tm) {
        snprintf(detail, sizeof detail, "NULL, errno=%d", errno);
        test_fail(name, detail);
        return;
    }
    snprintf(got, sizeof got, "%04ld-%02d-%02d %02d:%02d:%02d",
             tm->tm_year + 1900L, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour,
             tm->tm_min, tm->tm_sec);
    if (strcmp(got, want) || tm->tm_isdst != isdst || tm->tm_gmtoff != gmtoff ||
        !tm->tm_zone || strcmp(tm->tm_zone, zone)) {
        snprintf(detail, sizeof detail,
                 "got=[%s isdst=%d off=%ld %s] want=[%s isdst=%d off=%ld %s]",
                 got, tm->tm_isdst, tm->tm_gmtoff,
                 tm->tm_zone ? tm->tm_zone : "(null)", want, isdst, gmtoff,
                 zone);
        test_fail(name, detail);
        return;
    }
    test_ok(name);
}

static uint64_t now_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* Check that a call failed with EOVERFLOW. Use as expect_overflow(name, (errno
 * = 0, f(...)) == failure_value).
 */
static void expect_overflow(const char *name, int failed)
{
    check(failed && errno == EOVERFLOW, name,
          "expected failure with EOVERFLOW");
}

/* Average cost in ns of gmtime_r over n times from `base`, stepping by `step`.
 */
static uint64_t gmtime_avg_ns(long long base, long long step, int n)
{
    struct tm tm;
    uint64_t t0 = now_ns();

    for (int i = 0; i < n; i++) {
        time_t t = (time_t) (base + step * i);
        gmtime_r(&t, &tm);
    }
    return (now_ns() - t0) / (uint64_t) n;
}

static void test_gmtime_edges(void)
{
    static const struct {
        long long t;
        const char *want;
        int wday, yday;
    } cases[] = {
        {-1, "1969-12-31 23:59:59", 3, 364},
        {0, "1970-01-01 00:00:00", 4, 0},
        {951782400, "2000-02-29 00:00:00", 2, 59},
        {4107542400LL, "2100-03-01 00:00:00", 1, 59},
        {-11676096000LL, "1600-01-01 00:00:00", 6, 0},
        {-62135596800LL, "0001-01-01 00:00:00", 1, 0},
        {253402300799LL, "9999-12-31 23:59:59", 5, 364},
        {2147483648LL, "2038-01-19 03:14:08", 2, 18},
    };
    struct tm tm;
    char name[64];

    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        time_t t = (time_t) cases[i].t;
        struct tm *r = gmtime_r(&t, &tm);

        snprintf(name, sizeof name, "gmtime(%lld)", cases[i].t);
        expect_tm(name, r, cases[i].want, 0, 0, "UTC");
        if (r && (r->tm_wday != cases[i].wday || r->tm_yday != cases[i].yday))
            test_fail(name, "tm_wday or tm_yday");
    }

    {
        time_t max = INT64_MAX, min = INT64_MIN;

        expect_overflow("gmtime(INT64_MAX) is EOVERFLOW",
                        (errno = 0, gmtime(&max)) == NULL);
        expect_overflow("gmtime(INT64_MIN) is EOVERFLOW",
                        (errno = 0, gmtime_r(&min, &tm)) == NULL);
        expect_overflow("localtime(INT64_MAX) is EOVERFLOW",
                        (errno = 0, localtime_r(&max, &tm)) == NULL);
    }

    /* The year that just fits tm_year, and one past it. */
    {
        struct tm big = {.tm_year = INT_MAX,
                         .tm_mon = 11,
                         .tm_mday = 31,
                         .tm_hour = 23,
                         .tm_min = 59,
                         .tm_sec = 59};
        struct tm over = big;
        time_t t = timegm(&big);
        struct tm *r;

        r = gmtime_r(&t, &tm);
        check(t != (time_t) -1 && r && r->tm_year == INT_MAX &&
                  r->tm_mon == 11 && r->tm_mday == 31,
              "gmtime at tm_year INT_MAX",
              "the last representable second must convert");
        t++;
        expect_overflow("gmtime past tm_year INT_MAX",
                        (errno = 0, gmtime_r(&t, &tm)) == NULL);
        over.tm_sec = 60;
        expect_overflow("timegm past tm_year INT_MAX",
                        (errno = 0, timegm(&over)) == (time_t) -1);
    }
}

/* The conversion has no loop over years: the cost of a far year is the cost of
 * a near one. The budget is generous for an emulated guest; the point is that
 * it does not scale with the year (the old code took ~10^9 iterations here).
 */
static void test_gmtime_bound(void)
{
    enum { N = 2000 };
    uint64_t near_ns = gmtime_avg_ns(0, 1, N);
    uint64_t far_ns =
        gmtime_avg_ns(60000000000000000LL, 1, N); /* ~ year 1.9e9 */
    uint64_t max_ns = gmtime_avg_ns(INT64_MAX, 0, N);
    char detail[128];

    snprintf(detail, sizeof detail, "near=%lluns far=%lluns max=%lluns",
             (unsigned long long) near_ns, (unsigned long long) far_ns,
             (unsigned long long) max_ns);
    printf("  INFO  gmtime cost %s\n", detail);

    /* Bounded either absolutely or relative to a near year, so a preempted run
     * on a slow host does not fail a cost that does not grow.
     */
    check((far_ns < 50000 || far_ns < 4 * near_ns) &&
              (max_ns < 50000 || max_ns < 4 * near_ns),
          "gmtime cost is bounded", detail);
}

static void test_timegm_roundtrip(void)
{
    char detail[96];
    int bad = 0;
    long long first = 0;

    for (int i = 0; i < 20000 && !bad; i++) {
        struct tm tm;
        /* Half near the present, half across the whole representable range. */
        long long span = i < 10000 ? 100000000000LL : 60000000000000000LL;
        long long t = (long long) (rng() % (2ULL * (uint64_t) span)) - span;
        time_t tt = (time_t) t;

        if (!gmtime_r(&tt, &tm) || timegm(&tm) != tt) {
            bad = 1;
            first = t;
        }
    }
    snprintf(detail, sizeof detail, "first failure at t=%lld", first);
    check(!bad, "timegm(gmtime(t)) == t for 2x10^4 t", detail);

    /* Out-of-range fields are normalised. */
    {
        struct tm tm = {.tm_year = 99,
                        .tm_mon = 12,
                        .tm_mday = 0,
                        .tm_hour = 24,
                        .tm_min = -1,
                        .tm_sec = 60};
        time_t t = timegm(&tm);

        check(t == 946684800 && tm.tm_year == 100 && tm.tm_mon == 0 &&
                  tm.tm_mday == 1 && tm.tm_hour == 0 && tm.tm_min == 0 &&
                  tm.tm_sec == 0 && tm.tm_wday == 6 && tm.tm_yday == 0,
              "timegm normalises out-of-range fields",
              "1999-13-00 24:-1:60 is 2000-01-01");
    }
}

static void test_tz_parse(void)
{
    static const char *const invalid[] = {
        "garbage",
        "EST",
        "E5",
        "EST25",
        "EST5EDT,M13.1.0,M1.1.0",
        "EST5EDT,M3.2.0",
        "EST5EDT,M3.2.0,M11.1.0x",
        "EST5EDT,J0,J365",
        ":Nowhere/Missing",
        "Nowhere/Missing",
        "../zoneinfo/Europe/Berlin",
        "",
        "<EST5",
        "TOOLONGNAMEXXXXXXXXXXXXXXXXXXXXXXXXXXXXX5",
        "AAA5:60",
        "AAA0BBB,M3.2.0/168,M11.1.0",
    };
    char name[96];
    time_t t = 0;
    struct tm tm;

    set_tz(NULL);
    expect_tm("unset TZ is UTC", localtime_r(&t, &tm), "1970-01-01 00:00:00", 0,
              0, "UTC");
    check(!strcmp(tzname[0], "UTC") && timezone == 0 && daylight == 0,
          "unset TZ sets tzname UTC", "tzname/timezone/daylight");

    for (size_t i = 0; i < sizeof invalid / sizeof invalid[0]; i++) {
        set_tz(invalid[i]);
        snprintf(name, sizeof name, "invalid TZ \"%s\" is UTC", invalid[i]);
        check(localtime_r(&t, &tm) && tm.tm_gmtoff == 0 &&
                  !strcmp(tm.tm_zone, "UTC") && !strcmp(tzname[0], "UTC") &&
                  timezone == 0 && daylight == 0,
              name, "expected the UTC fallback");
    }

    set_tz(US_TZ);
    check(!strcmp(tzname[0], "EST") && !strcmp(tzname[1], "EDT") &&
              timezone == 18000 && daylight == 1,
          "TZ=" US_TZ " sets tzname/timezone/daylight", "EST/EDT 18000 1");

    set_tz("<+0530>-5:30");
    expect_tm("quoted name and minute offset", localtime_r(&t, &tm),
              "1970-01-01 05:30:00", 0, 19800, "+0530");

    /* Southern hemisphere: DST spans the new year. */
    set_tz("AEST-10AEDT,M10.1.0,M4.1.0/3");
    t = 1735689600; /* 2025-01-01 00:00 UTC */
    expect_tm("southern DST in January", localtime_r(&t, &tm),
              "2025-01-01 11:00:00", 1, 39600, "AEDT");
    t = 1751328000; /* 2025-07-01 00:00 UTC */
    expect_tm("southern standard time in July", localtime_r(&t, &tm),
              "2025-07-01 10:00:00", 0, 36000, "AEST");

    /* Julian rules: J60 is 1 March even in a leap year, 59 is 29 February. */
    set_tz("AAA0BBB,J60/0,59/0");
    t = 951782400; /* 2000-02-29 00:00 UTC */
    expect_tm("n59 is 29 February in a leap year", localtime_r(&t, &tm),
              "2000-02-29 00:00:00", 0, 0, "AAA");
    t = 951868800; /* 2000-03-01 00:00 UTC */
    expect_tm("J60 is 1 March in a leap year", localtime_r(&t, &tm),
              "2000-03-01 01:00:00", 1, 3600, "BBB");

    /* A rule time that moves a transition into the previous year: the 2025
     * start, J1/-1, falls at 2024-12-31 23:00.
     */
    set_tz("AAA0BBB,J1/-1,J200/0");
    t = 1735687800; /* 2024-12-31 23:30 UTC */
    expect_tm("transition in the previous year", localtime_r(&t, &tm),
              "2025-01-01 00:30:00", 1, 3600, "BBB");

    /* Component bounds: hours up to 24 (167 in a rule time), minutes and
     * seconds up to 59 each.
     */
    set_tz("AAA24:59:59");
    t = 0;
    check(localtime_r(&t, &tm) && tm.tm_gmtoff == -89999 &&
              !strcmp(tm.tm_zone, "AAA"),
          "TZ offset 24:59:59 accepted", "expected gmtoff -89999");
    set_tz("AAA0BBB,M3.2.0/167:59:59,M11.1.0/-167");
    check(daylight == 1, "rule time 167:59:59 accepted", "expected a DST zone");

    /* A tm_zone handed out stays valid after TZ changes. */
    {
        const char *zone;

        set_tz(US_TZ);
        t = 0;
        localtime_r(&t, &tm);
        zone = tm.tm_zone;
        set_tz("CET-1CEST,M3.5.0,M10.5.0/3");
        localtime_r(&t, &tm);
        check(!strcmp(zone, "EST") && !strcmp(tm.tm_zone, "CET"),
              "tm_zone outlives a TZ change", "old name rewritten");
    }

    set_tz("EST5");
    t = 0;
    {
        char buf[26];
        check(ctime_r(&t, buf) && !strcmp(buf, "Wed Dec 31 19:00:00 1969\n"),
              "ctime uses local time", "expected Wed Dec 31 19:00:00 1969");
    }
}

static void test_dst_edges(void)
{
    struct tm tm;
    time_t t;
    char detail[96];
    int bad = 0;
    long long first = 0;

    set_tz(US_TZ);

    t = (time_t) (SPRING_2025 - 1);
    expect_tm("last EST second before spring", localtime_r(&t, &tm),
              "2025-03-09 01:59:59", 0, -18000, "EST");
    t = (time_t) SPRING_2025;
    expect_tm("first EDT second", localtime_r(&t, &tm), "2025-03-09 03:00:00",
              1, -14400, "EDT");
    t = (time_t) (FALL_2025 - 1);
    expect_tm("last EDT second", localtime_r(&t, &tm), "2025-11-02 01:59:59", 1,
              -14400, "EDT");
    t = (time_t) FALL_2025;
    expect_tm("first EST second after fall", localtime_r(&t, &tm),
              "2025-11-02 01:00:00", 0, -18000, "EST");

    /* mktime(localtime(t)) == t across both edges, tm_isdst preserved, so both
     * sides of the fold round-trip.
     */
    for (long long edge = 0; edge < 2 && !bad; edge++) {
        long long base = edge ? FALL_2025 : SPRING_2025;

        for (long long d = -7200; d <= 7200 && !bad; d += 37) {
            struct tm lt;
            time_t in = (time_t) (base + d), out;

            localtime_r(&in, &lt);
            out = mktime(&lt);
            if (out != in) {
                bad = 1;
                first = base + d;
            }
        }
    }
    snprintf(detail, sizeof detail, "first failure at t=%lld", first);
    check(!bad, "mktime(localtime(t)) == t across DST edges", detail);

    /* Both sides of the fold: 01:30 happens twice on 2025-11-02. */
    {
        struct tm a = {.tm_year = 125,
                       .tm_mon = 10,
                       .tm_mday = 2,
                       .tm_hour = 1,
                       .tm_min = 30,
                       .tm_isdst = 1};
        struct tm b = a;

        b.tm_isdst = 0;
        check(mktime(&a) == (time_t) (FALL_2025 - 1800) && a.tm_isdst == 1,
              "fold 01:30 with tm_isdst=1 is EDT", "expected FALL-1800");
        check(mktime(&b) == (time_t) (FALL_2025 + 1800) && b.tm_isdst == 0,
              "fold 01:30 with tm_isdst=0 is EST", "expected FALL+1800");
        a.tm_hour = 1;
        a.tm_min = 30;
        a.tm_isdst = -1;
        check(mktime(&a) == (time_t) (FALL_2025 - 1800) && a.tm_isdst == 1,
              "fold 01:30 with tm_isdst=-1 is the earlier (EDT)",
              "expected FALL-1800");
    }

    /* The gap: 02:30 does not exist on 2025-03-09. tm_isdst=-1 reads it as EST
     * and so normalises forward to 03:30 EDT.
     */
    {
        static const struct tm gap = {.tm_year = 125,
                                      .tm_mon = 2,
                                      .tm_mday = 9,
                                      .tm_hour = 2,
                                      .tm_min = 30};
        struct tm g = gap;
        time_t r;

        g.tm_isdst = -1;
        r = mktime(&g);
        snprintf(detail, sizeof detail, "got t=%lld %02d:%02d isdst=%d",
                 (long long) r, g.tm_hour, g.tm_min, g.tm_isdst);
        check(r == (time_t) (SPRING_2025 + 1800) && g.tm_hour == 3 &&
                  g.tm_min == 30 && g.tm_isdst == 1,
              "gap 02:30 with tm_isdst=-1 normalises forward", detail);

        /* An explicit tm_isdst picks the offset: 02:30 EST is 03:30 EDT, and
         * 02:30 EDT is 01:30 EST.
         */
        g = gap;
        g.tm_isdst = 0;
        check(mktime(&g) == (time_t) (SPRING_2025 + 1800) && g.tm_hour == 3 &&
                  g.tm_isdst == 1,
              "gap 02:30 with tm_isdst=0 reads as EST", "expected 03:30 EDT");
        g = gap;
        g.tm_isdst = 1;
        check(mktime(&g) == (time_t) (SPRING_2025 - 1800) && g.tm_hour == 1 &&
                  g.tm_isdst == 0,
              "gap 02:30 with tm_isdst=1 reads as EDT", "expected 01:30 EST");
    }

    /* mktime overflow: day 40 of December of the last representable year. */
    {
        struct tm big = {.tm_year = INT_MAX, .tm_mon = 11, .tm_mday = 40};

        expect_overflow("mktime overflow is EOVERFLOW",
                        (errno = 0, mktime(&big)) == (time_t) -1);
    }

    check(difftime((time_t) 10, (time_t) 4) == 6.0 &&
              difftime((time_t) INT64_MAX, (time_t) -1) > 9.2e18,
          "difftime", "exact for small values and no overflow for large");
}

/* The name store is bounded: once it is full, a TZ with a new name selects UTC,
 * and names already stored keep working. Runs last, since it fills the store.
 */
static void test_tz_name_store(void)
{
    char tz[16];
    int i, filled = -1;

    for (i = 0; i < 64 && filled < 0; i++) {
        /* Distinct three-letter names: XAA, XAB, ... */
        snprintf(tz, sizeof tz, "X%c%c0", 'A' + i / 26, 'A' + i % 26);
        set_tz(tz);
        if (!strcmp(tzname[0], "UTC"))
            filled = i;
    }
    check(filled > 0, "TZ name store is bounded",
          "64 distinct names never selected UTC");
    set_tz("EST5");
    check(!strcmp(tzname[0], "EST"), "stored names survive a full store",
          "EST no longer parses");
}

/* Zone files, when the image ships /usr/share/zoneinfo: Europe/Berlin in winter
 * and summer, in the years the footer rule covers, and through mktime.
 */
static void test_zone_files(void)
{
    time_t t;
    struct tm tm;

    if (access("/usr/share/zoneinfo/Europe/Berlin", R_OK)) {
        test_skip("geographic time zones",
                  "no /usr/share/zoneinfo on this image");
        return;
    }
    set_tz("Europe/Berlin");
    check(!strcmp(tzname[0], "CET") && !strcmp(tzname[1], "CEST") &&
              timezone == -3600 && daylight == 1,
          "TZ=Europe/Berlin sets tzname/timezone/daylight", "CET/CEST -3600 1");
    t = 1736942400; /* 2025-01-15 12:00 UTC */
    expect_tm("Berlin winter", localtime_r(&t, &tm), "2025-01-15 13:00:00", 0,
              3600, "CET");
    t = 1752580800; /* 2025-07-15 12:00 UTC */
    expect_tm("Berlin summer", localtime_r(&t, &tm), "2025-07-15 14:00:00", 1,
              7200, "CEST");
    t = 2519510400LL; /* 2049-11-03 00:00 UTC, past the table: the footer rule
                       */
    expect_tm("Berlin by the footer rule", localtime_r(&t, &tm),
              "2049-11-03 01:00:00", 0, 3600, "CET");
    t = 1743296400; /* 2025-03-30 01:00 UTC, the spring-forward instant */
    expect_tm("Berlin at the spring transition", localtime_r(&t, &tm),
              "2025-03-30 03:00:00", 1, 7200, "CEST");
    t--;
    expect_tm("Berlin just before the spring transition", localtime_r(&t, &tm),
              "2025-03-30 01:59:59", 0, 3600, "CET");

    /* The fall-back fold, 2025-10-26 02:30 local, with each tm_isdst. */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 125;
    tm.tm_mon = 9;
    tm.tm_mday = 26;
    tm.tm_hour = 2;
    tm.tm_min = 30;
    tm.tm_isdst = 1;
    check(mktime(&tm) == 1761438600 && tm.tm_isdst == 1,
          "mktime in the fold, DST", "2025-10-26 00:30 UTC");
    tm.tm_isdst = 0;
    check(mktime(&tm) == 1761442200 && tm.tm_isdst == 0,
          "mktime in the fold, standard", "2025-10-26 01:30 UTC");

    /* A wrong tm_isdst away from any transition selects that offset, as for a
     * rule zone, and the result is normalized.
     */
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 125;
    tm.tm_mon = 6;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = 0;
    check(mktime(&tm) == 1752577200 && tm.tm_isdst == 1 && tm.tm_hour == 13,
          "mktime in summer with tm_isdst 0", "11:00 UTC, shown as 13:00 CEST");
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 125;
    tm.tm_mon = 0;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    tm.tm_isdst = 1;
    check(mktime(&tm) == 1736935200 && tm.tm_isdst == 0 && tm.tm_hour == 11,
          "mktime in winter with tm_isdst 1", "10:00 UTC, shown as 11:00 CET");

    set_tz(":Europe/Berlin");
    t = 1752580800;
    expect_tm("a leading colon names the file", localtime_r(&t, &tm),
              "2025-07-15 14:00:00", 1, 7200, "CEST");
}

void test_time(void)
{
    /* Restore the caller's TZ afterwards, not just unset it. */
    const char *env_tz = getenv("TZ");
    char *saved_tz = env_tz ? strdup(env_tz) : NULL;

    test_gmtime_edges();
    test_gmtime_bound();
    test_timegm_roundtrip();
    test_tz_parse();
    test_dst_edges();
    test_tz_name_store();
    test_zone_files();
    set_tz(saved_tz);
    free(saved_tz);
}
