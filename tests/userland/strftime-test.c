/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * strftime_test - the fixed C-locale vectors of strftime-cases.h, the size
 * edges (maxsize 0, an exact fit, truncation), malformed formats, and %s, %z
 * and %Z of a struct tm from localtime.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "strftime-cases.h"
#include "test.h"

static void expect_fmt(const char *name,
                       const struct tm *tm,
                       const char *fmt,
                       const char *want)
{
    char buf[512], detail[1200];
    size_t n = strftime(buf, sizeof buf, fmt, tm);

    if (n == strlen(want) && !strcmp(buf, want)) {
        test_ok(name);
        return;
    }
    snprintf(detail, sizeof detail, "fmt=[%s] n=%zu got=[%s] want=[%s]", fmt, n,
             buf, want);
    test_fail(name, detail);
}

/* buf is filled with a sentinel; strftime may write only buf[0..limit). */
static int untouched_from(const char *buf, size_t size, size_t limit)
{
    for (size_t i = limit; i < size; i++) {
        if (buf[i] != '#')
            return 0;
    }
    return 1;
}

static void test_size_edges(void)
{
    const struct tm *tm = &strftime_vecs[STRFTIME_VEC_EST].tm;
    char buf[32];
    size_t n;

    memset(buf, '#', sizeof buf);
    n = strftime(buf, 0, "x", tm);
    check(n == 0 && untouched_from(buf, sizeof buf, 0),
          "maxsize 0 writes nothing", "returned non-zero or wrote buf[0]");

    memset(buf, '#', sizeof buf);
    n = strftime(buf, 8, "ab%", tm);
    check(n == 0 && untouched_from(buf, sizeof buf, 8), "trailing % returns 0",
          "expected 0 and no write past maxsize");

    memset(buf, '#', sizeof buf);
    n = strftime(buf, 11, "%F", tm);
    check(n == 10 && !strcmp(buf, "2025-11-02") &&
              untouched_from(buf, sizeof buf, 11),
          "exact fit succeeds", "10 characters and the NUL fill 11 bytes");

    memset(buf, '#', sizeof buf);
    n = strftime(buf, 10, "%F", tm);
    check(n == 0 && untouched_from(buf, sizeof buf, 10),
          "one byte short returns 0", "expected 0 and no write past maxsize");

    memset(buf, '#', sizeof buf);
    n = strftime(buf, 5, "abcdefgh", tm);
    check(n == 0 && untouched_from(buf, sizeof buf, 5),
          "truncated literal returns 0",
          "expected 0 and no write past maxsize");

    memset(buf, '#', sizeof buf);
    n = strftime(buf, 12, "%c", tm);
    check(n == 0 && untouched_from(buf, sizeof buf, 12),
          "truncated %c returns 0", "expected 0 and no write past maxsize");

    memset(buf, '#', sizeof buf);
    n = strftime(buf, sizeof buf, "a%Qb", tm);
    check(n == 0, "unknown conversion returns 0", "expected 0");

    /* A width wider than the buffer pads only up to maxsize, then fails. */
    memset(buf, '#', sizeof buf);
    n = strftime(buf, 16, "%99999999999999999999Y", tm);
    check(n == 0 && untouched_from(buf, sizeof buf, 16), "huge width returns 0",
          "expected 0 and no write past maxsize");
}

static void test_zone_fields(void)
{
    struct tm tm = strftime_vecs[STRFTIME_VEC_EST].tm;

    tm.tm_isdst = -1;
    expect_fmt("%z and %Z are empty when tm_isdst < 0", &tm, "[%z][%Z]",
               "[][]");
    tm.tm_isdst = 0;
    tm.tm_zone = NULL;
    expect_fmt("%Z is empty for a NULL tm_zone", &tm, "[%Z]", "[]");

    /* A struct tm from localtime carries its zone into %s, %z and %Z. */
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    {
        time_t t = 1741503600; /* 2025-03-09 07:00:00 UTC */
        struct tm lt;

        localtime_r(&t, &lt);
        expect_fmt("localtime %s/%z/%Z", &lt, "%s %z %Z %F %T",
                   "1741503600 -0400 EDT 2025-03-09 03:00:00");
    }

    /* strftime acts as if it called tzset(), whatever the format. */
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    {
        char buf[8];

        strftime(buf, sizeof buf, "%Y", &strftime_vecs[0].tm);
        check(!strcmp(tzname[0], "CET") && !strcmp(tzname[1], "CEST"),
              "strftime updates tzname as tzset does", tzname[0]);
    }
    unsetenv("TZ");
    tzset();
}

void test_strftime(void)
{
    char name[48];

    for (size_t i = 0; i < sizeof strftime_vecs / sizeof strftime_vecs[0];
         i++) {
        snprintf(name, sizeof name, "strftime vector %zu", i);
        expect_fmt(name, &strftime_vecs[i].tm, STRFTIME_ALL,
                   strftime_vecs[i].want);
    }
    for (size_t i = 0;
         i < sizeof strftime_fmt_vecs / sizeof strftime_fmt_vecs[0]; i++) {
        const struct strftime_fmt_vec *v = &strftime_fmt_vecs[i];

        snprintf(name, sizeof name, "strftime format %zu", i);
        expect_fmt(name, &strftime_vecs[v->tm_index].tm, v->fmt, v->want);
    }
    for (size_t i = 0; i < sizeof strftime_tm_vecs / sizeof strftime_tm_vecs[0];
         i++) {
        snprintf(name, sizeof name, "strftime edge %zu", i);
        expect_fmt(name, &strftime_tm_vecs[i].tm, strftime_tm_vecs[i].fmt,
                   strftime_tm_vecs[i].want);
    }
    test_size_edges();
    test_zone_fields();
}
