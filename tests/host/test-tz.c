/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host test of the zone file support in lib/libc/posix/localtime.c and
 * tzfile.c, built with the time functions renamed to rtmux_* so the host libc
 * copies are the reference.
 *
 * Part one compares localtime and mktime with the host libc over 10^5 times
 * between 1970 and 2100 for Europe/Berlin, and over fewer for other zones, plus
 * every DST transition of those years to the second. It is skipped, with a
 * message, if the host has no zoneinfo for a zone. Part two writes fixture TZif
 * files and checks the footer, the fallbacks to UTC (missing, version 1,
 * truncated, malformed, unsafe names, too many files) and that a file that was
 * missing is not looked for again until TZ changes.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

struct tm *rtmux_localtime_r(const time_t *, struct tm *);
time_t rtmux_mktime(struct tm *);
void rtmux_tzset(void);
extern char *rtmux_tzname[2];
extern long rtmux_timezone;
extern int rtmux_daylight;

static int failures;
static long checks;

#define CHECK(cond, ...)                                                  \
    do {                                                                  \
        checks++;                                                         \
        if (!(cond)) {                                                    \
            failures++;                                                   \
            fprintf(stderr, "tz-test: FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                                 \
            fputc('\n', stderr);                                          \
        }                                                                 \
    } while (0)

static uint64_t rng_state = 0x9e3779b97f4a7c15ULL;

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static void use_tz(const char *tz)
{
    setenv("TZ", tz, 1);
    tzset();
    rtmux_tzset();
}

/* Whether the two broken-down times and zone fields agree. */
static int same_tm(const struct tm *a, const struct tm *b)
{
    return a->tm_sec == b->tm_sec && a->tm_min == b->tm_min &&
           a->tm_hour == b->tm_hour && a->tm_mday == b->tm_mday &&
           a->tm_mon == b->tm_mon && a->tm_year == b->tm_year &&
           a->tm_wday == b->tm_wday && a->tm_yday == b->tm_yday &&
           a->tm_isdst == b->tm_isdst && a->tm_gmtoff == b->tm_gmtoff &&
           a->tm_zone && b->tm_zone && !strcmp(a->tm_zone, b->tm_zone);
}

static int compare_at(const char *tz, time_t t)
{
    struct tm want, got;

    localtime_r(&t, &want);
    if (!rtmux_localtime_r(&t, &got) || !same_tm(&want, &got)) {
        fprintf(stderr,
                "tz-test: %s t=%lld: want %04d-%02d-%02d %02d:%02d:%02d "
                "isdst=%d off=%ld %s, got isdst=%d off=%ld %s h=%02d\n",
                tz, (long long) t, want.tm_year + 1900, want.tm_mon + 1,
                want.tm_mday, want.tm_hour, want.tm_min, want.tm_sec,
                want.tm_isdst, want.tm_gmtoff, want.tm_zone, got.tm_isdst,
                got.tm_gmtoff, got.tm_zone ? got.tm_zone : "?", got.tm_hour);
        return 0;
    }
    return 1;
}

/* Whether the wall time of t is read by no other instant within a day and a
 * half of it that is a whole number of minutes away, so that tm_isdst -1 has
 * one answer. Slow, for the instants around transitions only.
 */
static int wall_is_unique(time_t t)
{
    struct tm tm;
    long long wall;

    localtime_r(&t, &tm);
    wall = (long long) t + tm.tm_gmtoff;
    for (long k = -2160; k <= 2160; k++) {
        time_t u = t + (time_t) k * 60;

        if (k && (localtime_r(&u, &tm), (long long) u + tm.tm_gmtoff == wall))
            return 0;
    }
    return 1;
}

/* mktime of the broken-down time of t gives t back, with tm_isdst kept; where
 * the wall time has no other reading, with tm_isdst -1 too, and the host libc
 * agrees. `exact` finds such times by search, around transitions; otherwise
 * only times a day clear of any transition are taken.
 */
static int roundtrip_at(const char *tz, time_t t, int exact)
{
    struct tm tm, copy, a, b;
    time_t day_before = t - 86400, day_after = t + 86400;
    int unique;

    localtime_r(&t, &tm);
    copy = tm;
    if (rtmux_mktime(&copy) != t || !same_tm(&copy, &tm)) {
        fprintf(stderr, "tz-test: %s mktime(%lld) with isdst=%d gave %lld\n",
                tz, (long long) t, tm.tm_isdst, (long long) rtmux_mktime(&tm));
        return 0;
    }
    localtime_r(&day_before, &a);
    localtime_r(&day_after, &b);
    unique = exact ? wall_is_unique(t)
                   : a.tm_gmtoff == tm.tm_gmtoff && b.tm_gmtoff == tm.tm_gmtoff;
    if (unique) {
        struct tm x = tm, y = tm;

        x.tm_isdst = y.tm_isdst = -1;
        if (rtmux_mktime(&x) != t || mktime(&y) != t) {
            fprintf(stderr, "tz-test: %s mktime(%lld) with isdst=-1 differs\n",
                    tz, (long long) t);
            return 0;
        }
    }
    return 1;
}

static const time_t T1970 = 0;
static const time_t T2100 = 4102444800LL; /* 2100-01-01 */

/* Every transition between 1970 and 2100, found by scanning a day at a time and
 * bisecting on tm_gmtoff of the host libc, to the second.
 */
static void check_transitions(const char *tz)
{
    struct tm a, b;
    long count = 0;

    for (time_t t = T1970; t < T2100; t += 86400) {
        time_t lo = t, hi = t + 86400;

        localtime_r(&lo, &a);
        localtime_r(&hi, &b);
        if (a.tm_gmtoff == b.tm_gmtoff && a.tm_isdst == b.tm_isdst)
            continue;
        while (hi - lo > 1) {
            time_t mid = lo + (hi - lo) / 2;

            localtime_r(&mid, &b);
            if (b.tm_gmtoff == a.tm_gmtoff && b.tm_isdst == a.tm_isdst)
                lo = mid;
            else
                hi = mid;
        }
        for (time_t d = -1; d <= 1; d++) {
            CHECK(compare_at(tz, hi + d), "%s around transition %lld", tz,
                  (long long) hi);
            CHECK(roundtrip_at(tz, hi + d, 1),
                  "%s mktime around transition %lld", tz, (long long) hi);
        }
        count++;
    }
    CHECK(count > 0 || !strcmp(tz, "Asia/Tokyo") || !strcmp(tz, "Asia/Kolkata"),
          "%s: no transition found", tz);
}

static const char *rtmux_dir = "/usr/share/zoneinfo/";

static void compare_zone(const char *tz, long samples, int has_dst)
{
    char path[256];

    /* The libc reads RTMUX_TZDIR when set (the image's zone files), and the
     * host libc reads its own; a missing file in RTMUX_TZDIR is a failure.
     */
    snprintf(path, sizeof path, "%s%s", rtmux_dir, tz);
    if (access(path, R_OK)) {
        if (getenv("RTMUX_TZDIR")) {
            CHECK(0, "%s is missing from RTMUX_TZDIR", path);
        } else {
            printf("tz-test: SKIP %s (no %s)\n", tz, path);
        }
        return;
    }
    use_tz(tz);

    /* The host libc takes tzname[1] and daylight from historical types too, so
     * they are compared only for zones whose rule has DST.
     */
    CHECK(rtmux_timezone == timezone && !strcmp(rtmux_tzname[0], tzname[0]),
          "%s: tzname[0]/timezone: %s %ld vs %s %ld", tz, rtmux_tzname[0],
          rtmux_timezone, tzname[0], timezone);
    if (has_dst) {
        CHECK(rtmux_daylight == daylight && !strcmp(rtmux_tzname[1], tzname[1]),
              "%s: tzname[1]/daylight: %s %d vs %s %d", tz, rtmux_tzname[1],
              rtmux_daylight, tzname[1], daylight);
    }
    for (long i = 0; i < samples; i++) {
        time_t t = T1970 + (time_t) (rng() % (uint64_t) (T2100 - T1970));

        CHECK(compare_at(tz, t), "%s localtime at %lld", tz, (long long) t);
        CHECK(roundtrip_at(tz, t, 0), "%s mktime at %lld", tz, (long long) t);
    }
    check_transitions(tz);
}

/* Fixture zone files */

struct fix_type {
    int32_t off;
    int dst;
    int abbr; /* index into the abbreviation string */
};

static void put32(unsigned char **p, uint32_t v)
{
    for (int i = 3; i >= 0; i--)
        *(*p)++ = (unsigned char) (v >> (8 * i));
}

static void put64(unsigned char **p, uint64_t v)
{
    for (int i = 7; i >= 0; i--)
        *(*p)++ = (unsigned char) (v >> (8 * i));
}

static void put_header(unsigned char **p,
                       int version,
                       uint32_t timecnt,
                       uint32_t typecnt,
                       uint32_t charcnt,
                       uint32_t leapcnt)
{
    memcpy(*p, "TZif", 4);
    (*p)[4] = (unsigned char) version;
    memset(*p + 5, 0, 15);
    *p += 20;
    put32(p, 0); /* isutcnt */
    put32(p, 0); /* isstdcnt */
    put32(p, leapcnt);
    put32(p, timecnt);
    put32(p, typecnt);
    put32(p, charcnt);
}

struct fixture {
    int version; /* '2', or 0 for a version 1 file */
    int ntrans;
    int64_t times[4];
    unsigned char idx[4];
    int ntypes;
    struct fix_type types[3];
    const char *abbrs; /* NUL-separated, ends with an extra NUL */
    size_t abbr_len;
    const char *footer; /* NULL: none at all */
    int leap;           /* leap second records, each 12 zero bytes */
};

/* Write the fixture as TZif into buf; return its length. */
static size_t build(const struct fixture *f, unsigned char *buf)
{
    unsigned char *p = buf;

    /* The version 1 block: one type, no transitions. */
    put_header(&p, f->version ? f->version : '\0', 0, 1, 1, 0);
    put32(&p, 0);
    *p++ = 0;
    *p++ = 0;
    *p++ = 0;
    if (!f->version)
        return (size_t) (p - buf);
    put_header(&p, f->version, (uint32_t) f->ntrans, (uint32_t) f->ntypes,
               (uint32_t) f->abbr_len, (uint32_t) f->leap);
    for (int i = 0; i < f->ntrans; i++)
        put64(&p, (uint64_t) f->times[i]);
    for (int i = 0; i < f->ntrans; i++)
        *p++ = f->idx[i];
    for (int i = 0; i < f->ntypes; i++) {
        put32(&p, (uint32_t) f->types[i].off);
        *p++ = (unsigned char) f->types[i].dst;
        *p++ = (unsigned char) f->types[i].abbr;
    }
    memcpy(p, f->abbrs, f->abbr_len);
    p += f->abbr_len;
    memset(p, 0, 12 * (size_t) f->leap);
    p += 12 * (size_t) f->leap;
    if (f->footer) {
        *p++ = '\n';
        memcpy(p, f->footer, strlen(f->footer));
        p += strlen(f->footer);
        *p++ = '\n';
    }
    return (size_t) (p - buf);
}

static char fixdir[64];

static void write_zone(const char *name, const unsigned char *buf, size_t len)
{
    char path[160];
    FILE *fp;

    snprintf(path, sizeof path, "%s/%s", fixdir, name);
    fp = fopen(path, "wb");
    if (!fp || fwrite(buf, 1, len, fp) != len || fclose(fp)) {
        perror(path);
        exit(2);
    }
}

static void write_fixture(const char *name, const struct fixture *f)
{
    unsigned char buf[512];

    write_zone(name, buf, build(f, buf));
}

/* Zone "Fix/A": +1h "AAA" until 100000, then +2h DST "BBB" until 200000, then
 * +1h "AAA" until 300000, and after that the footer rule "CCC-3" (+3h, no DST).
 * The step up at 100000 skips the wall times 103600..107199, and the step down
 * at 200000 repeats 203600..207199.
 */
static const struct fixture fix_a = {
    .version = '2',
    .ntrans = 3,
    .times = {100000, 200000, 300000},
    .idx = {1, 0, 2},
    .ntypes = 3,
    .types = {{3600, 0, 0}, {7200, 1, 4}, {10800, 0, 8}},
    .abbrs = "AAA\0BBB\0CCC\0",
    .abbr_len = 12,
    .footer = "CCC-3"};

static int zone_is(time_t t, long off, int dst, const char *name)
{
    struct tm tm;

    return rtmux_localtime_r(&t, &tm) && tm.tm_gmtoff == off &&
           tm.tm_isdst == dst && !strcmp(tm.tm_zone, name);
}

static void expect_utc(const char *what, const char *tz)
{
    use_tz(tz);
    CHECK(zone_is(1000000, 0, 0, "UTC") && !strcmp(rtmux_tzname[0], "UTC") &&
              rtmux_timezone == 0 && rtmux_daylight == 0,
          "%s: TZ=%s is UTC", what, tz);
}

static void test_fixtures(void)
{
    unsigned char buf[512];
    struct fixture f;
    size_t len;
    char name[96];

    snprintf(fixdir, sizeof fixdir, "/tmp/rtmux-tz-%ld", (long) getpid());
    snprintf(name, sizeof name, "%s/Fix", fixdir);
    if (mkdir(fixdir, 0700) || mkdir(name, 0700)) {
        perror("mkdir");
        exit(2);
    }
    strcat(fixdir, "/");
    setenv("RTMUX_TZDIR", fixdir, 1);

    write_fixture("Fix/A", &fix_a);
    use_tz("Fix/A");
    CHECK(zone_is(-5, 3600, 0, "AAA"), "before the first transition: type 0");
    CHECK(zone_is(99999, 3600, 0, "AAA"), "just before a transition");
    CHECK(zone_is(100000, 7200, 1, "BBB"), "at a transition");
    CHECK(zone_is(199999, 7200, 1, "BBB"), "inside the DST interval");
    CHECK(zone_is(200000, 3600, 0, "AAA"), "back to standard time");
    CHECK(zone_is(299999, 3600, 0, "AAA"), "just before the last transition");
    CHECK(zone_is(300000, 10800, 0, "CCC"), "at the last transition");
    CHECK(zone_is(1LL << 40, 10800, 0, "CCC"), "far after the table: footer");
    CHECK(rtmux_daylight == 0 && rtmux_timezone == -10800 &&
              !strcmp(rtmux_tzname[0], "CCC"),
          "tzname/timezone from the footer: %s %ld %d", rtmux_tzname[0],
          rtmux_timezone, rtmux_daylight);
    use_tz(":Fix/A");
    CHECK(zone_is(100000, 7200, 1, "BBB"),
          "a leading colon names the file too");

    /* mktime in a zone file, in the fold and the gap, as for rule zones. */
    {
        struct tm tm;
        time_t wall;

        wall = 205000; /* in the fold: DST reading 197800, standard 201400 */
        gmtime_r(&wall, &tm);
        tm.tm_isdst = -1;
        CHECK(rtmux_mktime(&tm) == 197800 && tm.tm_isdst == 1,
              "isdst -1 in the fold reads as DST");
        gmtime_r(&wall, &tm);
        tm.tm_isdst = 1;
        CHECK(rtmux_mktime(&tm) == 197800, "isdst 1 in the fold");
        gmtime_r(&wall, &tm);
        tm.tm_isdst = 0;
        CHECK(rtmux_mktime(&tm) == 201400 && tm.tm_isdst == 0,
              "isdst 0 in the fold");

        wall = 105000; /* in the gap: read with the offset before it */
        gmtime_r(&wall, &tm);
        tm.tm_isdst = -1;
        CHECK(rtmux_mktime(&tm) == 101400 && tm.tm_isdst == 1,
              "a gap time is read as standard time");
        wall = 50000;
        gmtime_r(&wall, &tm);
        tm.tm_isdst = -1;
        CHECK(rtmux_mktime(&tm) == 50000 - 3600, "an ordinary time");
    }

    /* A file with no footer: the table's last type holds for ever. */
    f = fix_a;
    f.footer = "";
    write_fixture("Fix/B", &f);
    use_tz("Fix/B");
    CHECK(zone_is(1LL << 40, 10800, 0, "CCC"), "no footer: last type forever");
    CHECK(rtmux_daylight == 1 && !strcmp(rtmux_tzname[0], "CCC") &&
              !strcmp(rtmux_tzname[1], "BBB"),
          "no footer: tzname from the last types");

    /* A footer with DST rules covers every later year. */
    f = fix_a;
    f.footer = "XXX-1YYY,M3.5.0,M10.5.0/3";
    f.times[2] = 2000000000; /* 2033 */
    write_fixture("Fix/C", &f);
    use_tz("Fix/C");
    CHECK(zone_is(2205000000LL, 3600, 0, "XXX"), "footer standard time");
    CHECK(zone_is(2190000000LL, 7200, 1, "YYY"), "footer DST in mid-year");

    /* Rejected files select UTC. */
    expect_utc("missing file", "Fix/Missing");
    expect_utc("missing file with a colon", ":Fix/Missing");
    expect_utc("unsafe: parent", "Fix/../Fix/A");
    expect_utc("unsafe: absolute", "/etc/passwd");
    expect_utc("unsafe: empty component", "Fix//A");
    expect_utc("unsafe: hidden", "Fix/.A");
    expect_utc("unsafe: directory", "Fix/");
    {
        char big[200];

        memset(big, 'a', sizeof big - 1);
        big[sizeof big - 1] = '\0';
        expect_utc("name too long", big);
    }

    f = fix_a;
    f.version = 0;
    write_fixture("Fix/V1", &f);
    expect_utc("version 1 file", "Fix/V1");

    len = build(&fix_a, buf);
    for (size_t cut = 0; cut < len; cut += 7) {
        snprintf(name, sizeof name, "Fix/T%zu", cut);
        write_zone(name, buf, cut);
        expect_utc("truncated file", name);
    }
    write_zone("Fix/Empty", buf, 0);
    expect_utc("empty file", "Fix/Empty");

    memcpy(buf, "TZiF", 4);
    len = build(&fix_a, buf);
    memcpy(buf, "TZiF", 4);
    write_zone("Fix/Magic", buf, len);
    expect_utc("bad magic", "Fix/Magic");

    f = fix_a;
    f.times[1] = 100000;
    write_fixture("Fix/Order", &f);
    expect_utc("transitions not increasing", "Fix/Order");

    f = fix_a;
    f.idx[0] = 3;
    write_fixture("Fix/Idx", &f);
    expect_utc("type index out of range", "Fix/Idx");

    f = fix_a;
    f.types[1].abbr = 12;
    write_fixture("Fix/Abbr", &f);
    expect_utc("abbreviation index out of range", "Fix/Abbr");

    f = fix_a;
    f.abbrs = "AAA\0BBB\0CCCX";
    write_fixture("Fix/Term", &f);
    expect_utc("unterminated abbreviation", "Fix/Term");

    f = fix_a;
    f.types[0].off = 100000;
    write_fixture("Fix/Off", &f);
    expect_utc("offset out of range", "Fix/Off");

    f = fix_a;
    f.types[0].dst = 2;
    write_fixture("Fix/Dst", &f);
    expect_utc("bad isdst", "Fix/Dst");

    f = fix_a;
    f.leap = 1;
    write_fixture("Fix/Leap", &f);
    expect_utc("leap second records", "Fix/Leap");

    f = fix_a;
    f.footer = "not a rule";
    write_fixture("Fix/Foot", &f);
    expect_utc("malformed footer", "Fix/Foot");

    f = fix_a;
    f.footer = NULL;
    write_fixture("Fix/NoNl", &f);
    expect_utc("missing footer newlines", "Fix/NoNl");

    /* A file that appears later is found by the next tzset, not by a
     * conversion, which never looks for a file it did not find.
     */
    expect_utc("late file, first look", "Fix/Late");
    write_fixture("Fix/Late", &fix_a);
    CHECK(zone_is(1000000, 0, 0, "UTC"), "a conversion does not retry a failed lookup");
    rtmux_tzset();
    CHECK(zone_is(100000, 7200, 1, "BBB"), "tzset finds a file that appeared");
    CHECK(zone_is(1000000, 10800, 0, "CCC"), "and keeps it");

    /* A file that is still missing stays UTC after a retry. */
    expect_utc("bad name, first look", "Fix/Nope");
    rtmux_tzset();
    CHECK(zone_is(1000000, 0, 0, "UTC"), "a file that is still missing stays UTC");
    use_tz("");
    use_tz("Fix/Late");
    CHECK(zone_is(100000, 7200, 1, "BBB"), "found again after TZ changes");
}

/* The footer takes over strictly after the last transition: at the transition
 * itself the table's type holds, even if the footer says otherwise.
 */
static void test_footer_boundary(void)
{
    struct fixture f = fix_a;

    f.footer = "FFF-5";
    write_fixture("Fix/Foot2", &f);
    use_tz("Fix/Foot2");
    CHECK(zone_is(300000, 10800, 0, "CCC"), "the last transition's own type");
    CHECK(zone_is(300001, 18000, 0, "FFF"), "the footer one second later");
}

/* Offsets up to 26 hours and a step down of 1 hour: wall times that the
 * transition makes repeat, and a non-DST fold.
 */
static void test_large_offsets(void)
{
    static const struct fixture f = {.version = '2',
                                     .ntrans = 1,
                                     .times = {100000},
                                     .idx = {1},
                                     .ntypes = 2,
                                     .types = {{93600, 0, 0}, {90000, 0, 4}},
                                     .abbrs = "AAA\0BBB\0",
                                     .abbr_len = 8,
                                     .footer = ""};
    struct tm tm;
    time_t wall;

    write_fixture("Fix/Big", &f);
    use_tz("Fix/Big");
    CHECK(zone_is(99999, 93600, 0, "AAA"), "26 hours east, before the step");
    CHECK(zone_is(100000, 90000, 0, "BBB"), "25 hours east, after the step");

    /* Wall 193599 is read at +26h as UTC 99999, and at +25h as 103599: the
     * earlier reading wins, with isdst -1 or 0.
     */
    wall = 193599;
    gmtime_r(&wall, &tm);
    tm.tm_isdst = -1;
    CHECK(rtmux_mktime(&tm) == 99999, "wall time repeated by a 1 h step down");
    gmtime_r(&wall, &tm);
    tm.tm_isdst = 0;
    CHECK(rtmux_mktime(&tm) == 99999, "the same, with isdst 0");
    wall = 195000; /* only after the step: 195000 - 90000 */
    gmtime_r(&wall, &tm);
    tm.tm_isdst = -1;
    CHECK(rtmux_mktime(&tm) == 105000, "a wall time after the repeat");
}

/* A type in force for 100 seconds, between samples of the offsets near a wall
 * time: UTC, then +1h from 100 to 200, then UTC again. Wall 3750 reads as UTC
 * 150 (+1h) and as 3750 (UTC), and the earlier reading wins.
 */
static void test_short_lived_type(void)
{
    static const struct fixture f = {.version = '2',
                                     .ntrans = 2,
                                     .times = {100, 200},
                                     .idx = {1, 0},
                                     .ntypes = 2,
                                     .types = {{0, 0, 0}, {3600, 1, 4}},
                                     .abbrs = "AAA\0BBB\0",
                                     .abbr_len = 8,
                                     .footer = ""};
    struct tm tm;
    time_t wall = 3750;

    write_fixture("Fix/Short", &f);
    use_tz("Fix/Short");
    gmtime_r(&wall, &tm);
    tm.tm_isdst = -1;
    CHECK(rtmux_mktime(&tm) == 150 && tm.tm_isdst == 1,
          "a type in force for 100 seconds is found");
}

/* Zone files are kept as a list, so no count of files fails a lookup: 200
 * distinct names all work, and tm_zone pointers from the first load stay valid.
 */
static void test_many_files(void)
{
    enum { MANY = 200 };
    char name[96];
    struct tm tm;
    time_t t = 150000;
    const char *kept;
    int bad = 0;

    use_tz("Fix/A");
    rtmux_localtime_r(&t, &tm);
    kept = tm.tm_zone;
    for (int i = 0; i < MANY; i++) {
        snprintf(name, sizeof name, "Fix/N%d", i);
        write_fixture(name, &fix_a);
        use_tz(name);
        bad += !zone_is(150000, 7200, 1, "BBB");
    }
    CHECK(!bad, "%d of %d distinct zone files failed to load", bad, MANY);
    for (int i = 0; i < MANY; i += 17) {
        snprintf(name, sizeof name, "Fix/N%d", i);
        use_tz(name);
        bad += !zone_is(150000, 7200, 1, "BBB");
    }
    CHECK(!bad, "a kept file still works after many others");
    use_tz("Fix/A");
    CHECK(zone_is(150000, 7200, 1, "BBB"), "the first file still works");
    CHECK(!strcmp(kept, "BBB"), "tm_zone from an earlier load stays valid");
}

static void remove_fixtures(void)
{
    char cmd[128];

    fixdir[strlen(fixdir) - 1] = '\0';
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", fixdir);
    if (system(cmd))
        fprintf(stderr, "tz-test: could not remove %s\n", fixdir);
}

int main(void)
{
    if (getenv("RTMUX_TZDIR"))
        rtmux_dir = getenv("RTMUX_TZDIR");
    compare_zone("Europe/Berlin", 100000, 1);
    compare_zone("America/New_York", 20000, 1);
    compare_zone("Australia/Sydney", 20000, 1);
    compare_zone("Asia/Kolkata", 5000, 0);
    compare_zone("Pacific/Auckland", 20000, 1);
    unsetenv("TZ");
    test_fixtures();
    test_footer_boundary();
    test_large_offsets();
    test_short_lived_type();
    test_many_files();
    remove_fixtures();
    printf("tz-test: %s (%ld checks, %d failures)\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
