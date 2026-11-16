/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0 AND MIT
 */

/*
 * gmtime, localtime, mktime, timegm, tzset and the POSIX TZ rule parser.
 *
 * The zone lookup, mktime and timegm follow musl's __tz.c, mktime.c and
 * timegm.c. TZ is read in two forms. The POSIX rule form needs no files:
 *
 *   std offset [dst [offset] [,rule,rule]]
 *
 * Any other value, or one that starts with a colon, names a zone file, read
 * from /usr/share/zoneinfo (tzfile.c); see "Zone files" below.
 *
 * Bounds: a zone name is 3 to TZNAME_MAX bytes, alphabetic or <quoted> with
 * alphanumerics, '+' and '-'; an offset is [+-]hh[:mm[:ss]] with hh at most 24
 * and mm, ss at most 59; a rule is Jn (1-365), n (0-365) or Mm.w.d (1-12, 1-5,
 * 0-6), with an optional /time of [+-]hhh[:mm[:ss]], hhh at most 167. A DST
 * name without rules uses M3.2.0,M11.1.0. A TZ longer than TZ_STR_MAX bytes,
 * one that is neither a valid rule string nor the name of a readable zone file,
 * and an unset or empty TZ all select UTC; tzname[0] and tzname[1] are then
 * "UTC".
 *
 * Zone files: a TZif version 2+ file gives the local time type of each instant
 * up to its last transition, and its POSIX footer rule gives every later one,
 * through the same rule code as a TZ rule string. A name is relative to the
 * zone directory and may not contain "." or ".." components. A file is read
 * once, outside tz_lock, and kept for the life of the process, so tm_zone
 * pointers stay valid; at most TZ_FILES_MAX distinct files are kept, and a TZ
 * naming another selects UTC. A file that is missing when TZ is first seen is
 * not looked for again until TZ changes.
 *
 * Thread safety: the parsed zone is shared state under tz_lock, a priority-
 * inheritance mutex, and every conversion reads TZ again, so a setenv takes
 * effect at the next call. TZ is read and parsed outside tz_lock, the read
 * bounded to TZ_STR_MAX + 1 bytes and the parse run only when TZ changed; the
 * lock covers a bounded compare, the interning of at most two names among
 * TZ_NAMES_MAX, and a copy of the zone, never a conversion. As POSIX allows,
 * changing the environment while another thread converts a time is undefined.
 * Zone names are interned and never freed, so a tm_zone or tzname[] pointer
 * stays valid after TZ changes; once TZ_NAMES_MAX distinct names have been
 * seen, a TZ with a new name selects UTC. tzname, timezone and daylight are
 * plain objects written under tz_lock; a reader racing a TZ change may see a
 * mix of the old and new zone.
 *
 * Cost: a conversion computes the transitions of three rule years (so a rule
 * whose time crosses a year boundary is honoured) and never loops over years.
 *
 * Gap and fold: mktime with tm_isdst < 0 reads a local time that a spring-
 * forward transition skips as standard time, so 02:30 in a 02:00->03:00 gap
 * becomes 03:30 DST. An ambiguous time in a fall-back fold reads as DST, the
 * earlier of the two instants. A tm_isdst >= 0 selects that offset explicitly.
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
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "time-conv.h"
#include "tzfile.h"

#define TZ_STR_MAX TZFILE_FOOTER_MAX /* a footer is a TZ string */

/* Distinct zone names kept for the life of the process. */
#define TZ_NAMES_MAX 32

/* Distinct zone files kept for the life of the process, each about 3 KiB. */
#define TZ_FILES_MAX 64

#ifndef TZNAME_MAX
#define TZNAME_MAX 255
#endif

/* Seconds of the mean Gregorian year, for a first guess at the year of t. */
#define SECS_PER_MEAN_YEAR 31556952LL

/* A transition rule: kind 'J', 'M' or 'n' (zero-based day), its fields, and the
 * local wall time of the change in seconds after midnight.
 */
struct tz_rule {
    int kind;
    int m, w, d; /* M: month, week, weekday; J and n: day in d */
    long time;
};

struct tz_loaded;

/* A parsed TZ. Offsets are seconds west of UTC, as in `timezone`. The names are
 * interned; dst_name equals std_name without DST. For a zone file, `loaded` is
 * its data and the rest summarizes the footer, or the last types of the table;
 * for a rule string it is NULL.
 */
struct tz_zone {
    const char *std_name, *dst_name;
    long std_off, dst_off;
    int daylight;
    struct tz_rule start, end;
    const struct tz_loaded *loaded;
};

/* A zone file kept for the life of the process, and its parsed footer. */
struct tz_loaded {
    char name[TZ_STR_MAX + 1];
    const struct tzfile *file;
    int has_footer;
    struct tz_zone footer; /* the footer rule; names point at footer_std/dst */
    char footer_std[TZNAME_MAX + 1], footer_dst[TZNAME_MAX + 1];
    struct tz_zone zone; /* the summary tz_snapshot hands out */
};

char *tzname[2] = {(char *) "UTC", (char *) "UTC"};
long timezone;
int daylight;

static const char utc_name[] = "UTC";
static const struct tz_zone utc_zone = {.std_name = utc_name,
                                        .dst_name = utc_name};

/* Priority inheritance (PRIONONE clear, as for the malloc lock), so a preempted
 * low-priority holder cannot stall a high-priority conversion.
 */
#ifdef RTMUX_SYNC_INITIALIZER
static pthread_mutex_t tz_lock = {RTMUX_SYNC_NONRECURSIVE,
                                  RTMUX_SYNC_INITIALIZER};
#else
static pthread_mutex_t tz_lock = PTHREAD_MUTEX_INITIALIZER; /* host tests */
#endif
/* TZ as last parsed; the initial "" matches the initial UTC `zone`. */
static char tz_cached[TZ_STR_MAX + 1];
static struct tz_zone zone = {.std_name = utc_name, .dst_name = utc_name};

/* Interned zone names. Slots are never reused, so names handed out in tm_zone
 * and tzname[] outlive any later change of TZ.
 */
static char tz_names[TZ_NAMES_MAX][TZNAME_MAX + 1];
static int tz_name_count;

/* Zone files kept so far, newest last; an entry is complete before it is
 * published, under tz_lock, and is never freed or changed.
 */
static struct tz_loaded *tz_files[TZ_FILES_MAX];
static int tz_file_count;

/* Parse up to `maxdig` digits, at least one, into *out. */
static int get_num(const char **p, int maxdig, int *out)
{
    int v = 0, n = 0;

    while (isdigit(**p) && n < maxdig) {
        v = v * 10 + (**p - '0');
        ++*p;
        n++;
    }
    if (!n || isdigit(**p))
        return -1;
    *out = v;
    return 0;
}

/* [+-]hh[:mm[:ss]], hh at most max_hours and mm, ss at most 59. Positive means
 * west of UTC for a zone offset, and after midnight for a rule time.
 */
static int get_off(const char **p, int max_hours, int hour_digits, long *out)
{
    int neg = 0, h, m = 0, s = 0;
    long v;

    if (**p == '+' || **p == '-')
        neg = *(*p)++ == '-';
    if (get_num(p, hour_digits, &h) || h > max_hours)
        return -1;
    if (**p == ':') {
        ++*p;
        if (get_num(p, 2, &m) || m > 59)
            return -1;
        if (**p == ':') {
            ++*p;
            if (get_num(p, 2, &s) || s > 59)
                return -1;
        }
    }
    v = 3600L * h + 60L * m + s;
    *out = neg ? -v : v;
    return 0;
}

/* Parse a zone name into d (TZNAME_MAX + 1 bytes). */
static int get_name(const char **p, char *d)
{
    const char *s = *p;
    size_t n = 0;

    if (*s == '<') {
        s++;
        while (n <= TZNAME_MAX && (isalnum(s[n]) || s[n] == '+' || s[n] == '-'))
            n++;
        if (s[n] != '>')
            return -1;
        *p = s + n + 1;
    } else {
        while (n <= TZNAME_MAX && isalpha(s[n]))
            n++;
        *p = s + n;
    }
    if (n < 3 || n > TZNAME_MAX)
        return -1;
    memcpy(d, s, n);
    d[n] = '\0';
    return 0;
}

static int get_rule(const char **p, struct tz_rule *r)
{
    r->time = 7200;
    if (**p == 'M') {
        ++*p;
        r->kind = 'M';
        if (get_num(p, 2, &r->m) || r->m < 1 || r->m > 12 || *(*p)++ != '.' ||
            get_num(p, 1, &r->w) || r->w < 1 || r->w > 5 || *(*p)++ != '.' ||
            get_num(p, 1, &r->d) || r->d > 6)
            return -1;
    } else {
        r->kind = 'n';
        if (**p == 'J') {
            ++*p;
            r->kind = 'J';
        }
        if (get_num(p, 3, &r->d) || r->d > 365 || (r->kind == 'J' && r->d < 1))
            return -1;
    }
    if (**p == '/') {
        ++*p;
        if (get_off(p, 167, 3, &r->time))
            return -1;
    }
    return 0;
}

/* The stored copy of `name`, or NULL. Caller holds tz_lock; the search is
 * bounded by TZ_NAMES_MAX.
 */
static const char *find_name(const char *name)
{
    if (!strcmp(name, utc_name))
        return utc_name;
    for (int i = 0; i < tz_name_count; i++) {
        if (!strcmp(tz_names[i], name))
            return tz_names[i];
    }
    return NULL;
}

/* The stored copy of `name`, adding it if new. Caller holds tz_lock and has
 * checked that a slot is free.
 */
static const char *intern(const char *name)
{
    const char *found = find_name(name);

    if (found)
        return found;
    strcpy(tz_names[tz_name_count], name);
    return tz_names[tz_name_count++];
}

/* Parse a POSIX TZ rule string into *z, with the names in std and dst
 * (TZNAME_MAX + 1 bytes each) and not yet interned.
 */
static int parse_tz(const char *s, struct tz_zone *z, char *std, char *dst)
{
    if (get_name(&s, std) || get_off(&s, 24, 2, &z->std_off))
        return -1;
    z->daylight = 0;
    z->dst_off = z->std_off;
    dst[0] = '\0';
    if (*s) {
        if (get_name(&s, dst))
            return -1;
        z->daylight = 1;
        z->dst_off = z->std_off - 3600;
        if (*s && *s != ',' && get_off(&s, 24, 2, &z->dst_off))
            return -1;
        if (!*s) {
            /* No rules: the United States rules POSIX leaves to the system. */
            z->start = (struct tz_rule){
                .kind = 'M', .m = 3, .w = 2, .d = 0, .time = 7200};
            z->end = (struct tz_rule){
                .kind = 'M', .m = 11, .w = 1, .d = 0, .time = 7200};
        } else if (*s++ != ',' || get_rule(&s, &z->start) || *s++ != ',' ||
                   get_rule(&s, &z->end) || *s) {
            return -1;
        }
    }
    return 0;
}

/* Length of s if it is at most TZ_STR_MAX bytes, else TZ_STR_MAX + 1; reads no
 * further than that.
 */
static size_t tz_len(const char *s)
{
    size_t n = 0;

    while (n <= TZ_STR_MAX && s[n])
        n++;
    return n;
}

/* The kept zone file named `name`, or NULL. Caller holds tz_lock. */
static const struct tz_loaded *find_loaded(const char *name)
{
    for (int i = 0; i < tz_file_count; i++) {
        if (!strcmp(tz_files[i]->name, name))
            return tz_files[i];
    }
    return NULL;
}

/* Set the summary of *ld, and the footer's names, once its file and footer are
 * in. The summary's names and offsets are the footer's or, without a footer,
 * those of the last standard and DST types of the table.
 */
static void relink(struct tz_loaded *ld)
{
    struct tz_zone *z = &ld->zone;

    *z = (struct tz_zone){.loaded = ld};
    if (ld->has_footer) {
        ld->footer.std_name = ld->footer_std;
        ld->footer.dst_name =
            ld->footer.daylight ? ld->footer_dst : ld->footer_std;
        z->std_name = ld->footer.std_name;
        z->dst_name = ld->footer.dst_name;
        z->std_off = ld->footer.std_off;
        z->dst_off = ld->footer.dst_off;
        z->daylight = ld->footer.daylight;
    } else {
        struct tzfile_type std, dst;

        __tzfile_last_types(ld->file, &std, &dst, &z->daylight);
        z->std_name = std.name;
        z->dst_name = dst.name;
        z->std_off = -std.gmtoff;
        z->dst_off = -dst.gmtoff;
    }
}

/* Read the zone file `name` and parse its footer into a new *ld; NULL if it is
 * missing, invalid, has an invalid footer, or memory is short.
 */
static struct tz_loaded *load_zone(const char *name)
{
    struct tz_loaded *ld = calloc(1, sizeof *ld);
    const char *footer;

    if (!ld)
        return NULL;
    ld->file = __tzfile_load(name);
    if (!ld->file)
        goto fail;
    strcpy(ld->name, name);
    footer = __tzfile_footer(ld->file);
    if (*footer) {
        if (parse_tz(footer, &ld->footer, ld->footer_std, ld->footer_dst))
            goto fail;
        ld->has_footer = 1;
    }
    relink(ld);
    return ld;
fail:
    free((void *) ld->file);
    free(ld);
    return NULL;
}

/* The zone file TZ names (without a leading colon), kept on first use; NULL if
 * there is none or no room for another. The scan under the lock is bounded by
 * TZ_FILES_MAX; the read and parse run outside it. Threads that first see a
 * name together each read it, and the extra copies are freed.
 */
static const struct tz_loaded *get_loaded(const char *name)
{
    struct tz_loaded *fresh;
    const struct tz_loaded *ld;

    if (!__tzfile_name_ok(name))
        return NULL;
    pthread_mutex_lock(&tz_lock);
    ld = find_loaded(name);
    pthread_mutex_unlock(&tz_lock);
    if (ld || !(fresh = load_zone(name)))
        return ld;

    pthread_mutex_lock(&tz_lock);
    ld = find_loaded(name);
    if (!ld && tz_file_count < TZ_FILES_MAX) {
        tz_files[tz_file_count++] = fresh;
        ld = fresh;
        fresh = NULL;
    }
    pthread_mutex_unlock(&tz_lock);
    if (fresh) { /* lost a race, or no room */
        free((void *) fresh->file);
        free(fresh);
    }
    return ld;
}

/* Copy the zone that TZ names into *out. TZ is parsed, outside tz_lock, only
 * when it differs from the last one; the lock covers a bounded compare, the
 * interning and the copy. A zone file is read the first time TZ names it, which
 * tzset() does ahead of time for a program that must not do file I/O later.
 */
static void tz_snapshot(struct tz_zone *out)
{
    const char *s = getenv("TZ");
    char std[TZNAME_MAX + 1], dst[TZNAME_MAX + 1];
    const struct tz_loaded *ld = NULL;
    struct tz_zone z;
    size_t len;
    int ok;

    if (!s)
        s = "";
    len = tz_len(s);
    if (len > TZ_STR_MAX) {
        /* Too long to be valid: UTC, the same as an empty TZ. */
        s = "";
        len = 0;
    }

    pthread_mutex_lock(&tz_lock);
    if (!memcmp(s, tz_cached, len + 1)) {
        *out = zone;
        pthread_mutex_unlock(&tz_lock);
        return;
    }
    pthread_mutex_unlock(&tz_lock);

    ok = *s && *s != ':' && !parse_tz(s, &z, std, dst);
    if (!ok && *s) {
        /* Not a rule string: a zone file, with or without the colon. */
        ld = get_loaded(s + (*s == ':'));
    }

    pthread_mutex_lock(&tz_lock);
    if (ok) {
        /* Store both names or neither, so a failed TZ wastes no slot. */
        int need = !find_name(std) +
                   (z.daylight && strcmp(dst, std) && !find_name(dst));

        ok = tz_name_count + need <= TZ_NAMES_MAX;
    }
    if (ok) {
        z.std_name = intern(std);
        z.dst_name = z.daylight ? intern(dst) : z.std_name;
        z.loaded = NULL;
    }
    if (!ok)
        z = ld ? ld->zone : utc_zone;
    memcpy(tz_cached, s, len + 1);
    zone = z;
    tzname[0] = (char *) z.std_name;
    tzname[1] = (char *) z.dst_name;
    timezone = z.std_off;
    daylight = z.daylight;
    *out = z;
    pthread_mutex_unlock(&tz_lock);
}

void tzset(void)
{
    struct tz_zone z;

    tz_snapshot(&z);
}

static long long floor_div(long long a, long long b)
{
    long long q = a / b;

    return (a % b < 0) ? q - 1 : q;
}

static int days_in_month(int m, int is_leap)
{
    if (m == 2)
        return 28 + is_leap;
    return 30 + ((0xad5 >> (m - 1)) & 1);
}

/* The local wall time, counted as if it were UTC, at which rule r fires in the
 * year that starts at `year_secs`.
 */
static long long rule_to_secs(const struct tz_rule *r,
                              long long year_secs,
                              int is_leap)
{
    long long t = year_secs;

    if (r->kind == 'M') {
        int wday, days, n = r->w;

        t += __month_to_secs(r->m - 1, is_leap);
        wday = (int) ((floor_div(t, 86400) + 4) %
                      7); /* 1970-01-01 was a Thursday */
        if (wday < 0)
            wday += 7;
        days = r->d - wday;
        if (days < 0)
            days += 7;
        if (n == 5 && days + 28 >= days_in_month(r->m, is_leap))
            n = 4;
        t += 86400LL * (days + 7 * (n - 1));
    } else {
        int x = r->d;

        /* Jn counts 1-365 and never names 29 February. */
        if (r->kind == 'J' && (x < 60 || !is_leap))
            x--;
        t += 86400LL * x;
    }
    return t + r->time;
}

/* Whether DST is in effect at UTC time t: the kind of the latest transition at
 * or before t among the rule years around t. Three years cover a rule time up
 * to 167 hours past either end of its year.
 */
static int utc_is_dst(const struct tz_zone *z, long long t)
{
    long long local = t - z->std_off;
    long long y = floor_div(local, SECS_PER_MEAN_YEAR) + 70;
    long long last = LLONG_MIN;
    int dst = 0;

    for (long long yy = y - 1; yy <= y + 1; yy++) {
        int is_leap;
        long long base = __year_to_secs(yy, &is_leap);
        /* The start rule is in standard local time, the end rule in DST. */
        long long t0 = rule_to_secs(&z->start, base, is_leap) + z->std_off;
        long long t1 = rule_to_secs(&z->end, base, is_leap) + z->dst_off;

        if (t0 <= t && t0 >= last) {
            last = t0;
            dst = 1;
        }
        if (t1 <= t && t1 >= last) {
            last = t1;
            dst = 0;
        }
    }
    return dst;
}

/* Whether DST is in effect at t: UTC seconds, or, with `local`, local wall
 * seconds counted as if UTC.
 */
static int zone_is_dst(const struct tz_zone *z, long long t, int local)
{
    if (!z->daylight)
        return 0;

    /* A wall time read as DST that lands in DST is DST; that covers the fold,
     * and leaves a gap time to standard time (see the top of file).
     */
    return utc_is_dst(z, local ? t + z->dst_off : t);
}

/* Seconds east of UTC, as in tm_gmtoff. */
static long zone_gmtoff(const struct tz_zone *z, int dst)
{
    return dst ? -z->dst_off : -z->std_off;
}

/* The local time type of rule zone z (a TZ rule string, or a footer) at UTC t.
 */
static void rule_info(const struct tz_zone *z,
                      long long t,
                      struct tzfile_type *i)
{
    i->dst = zone_is_dst(z, t, 0);
    i->gmtoff = zone_gmtoff(z, i->dst);
    i->name = i->dst ? z->dst_name : z->std_name;
}

/* The local time type of zone z at UTC time t. */
static void zone_info(const struct tz_zone *z,
                      long long t,
                      struct tzfile_type *i)
{
    const struct tz_loaded *ld = z->loaded;

    if (!ld) {
        rule_info(z, t, i);
    } else if (ld->has_footer && __tzfile_after_table(ld->file, t)) {
        rule_info(&ld->footer, t, i);
    } else {
        __tzfile_type_at(ld->file, t, i);
    }
}

/* Fill *tm with UTC time t shown with local time type *i; NULL with EOVERFLOW
 * if the year does not fit.
 */
static struct tm *fill_tm(long long t,
                          const struct tzfile_type *i,
                          struct tm *tm)
{
    if (__secs_to_tm(t + i->gmtoff, tm) < 0) {
        errno = EOVERFLOW;
        return NULL;
    }
    tm->tm_isdst = i->dst;
    tm->tm_gmtoff = i->gmtoff;
    tm->tm_zone = i->name;
    return tm;
}

static struct tm result;

struct tm *gmtime_r(const time_t *tp, struct tm *tm)
{
    static const struct tzfile_type utc = {0, 0, utc_name};

    return fill_tm(*tp, &utc, tm);
}

struct tm *gmtime(const time_t *tp)
{
    return gmtime_r(tp, &result);
}

struct tm *localtime_r(const time_t *tp, struct tm *tm)
{
    struct tz_zone z;
    struct tzfile_type info;

    /* The zone lookup computes the year of t, so reject a year that does not
     * fit before it.
     */
    if (!__secs_in_range(*tp)) {
        errno = EOVERFLOW;
        return NULL;
    }
    tz_snapshot(&z);
    zone_info(&z, *tp, &info);
    return fill_tm(*tp, &info, tm);
}

struct tm *localtime(const time_t *tp)
{
    return localtime_r(tp, &result);
}

/* Largest offset a zone file may use, and the spacing of the instants sampled
 * for the offsets in force near a wall time. A zone does not change offset
 * twice within a spacing, and every offset in force within a day and a half of
 * the wall time is seen.
 */
#define OFF_REACH UTOFF_MAX
#define OFF_STEP 10800
#define OFF_SAMPLES (2 * OFF_REACH / OFF_STEP + 2)

/* Most table transitions within the window that are looked at; a zone file with
 * more than this in 52 hours has the extra ones ignored here.
 */
#define OFF_TABLE 16

/* Whether offset a, with mismatch ma (0 when consistent with the table), is a
 * better reading of a wall time than b: a consistent one over one that is not,
 * of two consistent ones the larger (the earlier instant), of two that are not
 * the nearer to fitting and then the smaller.
 */
static int offset_better(const struct tzfile_type *a,
                         long ma,
                         const struct tzfile_type *b,
                         long mb)
{
    if (!ma != !mb)
        return !ma;
    if (!ma)
        return a->gmtoff > b->gmtoff;
    return ma < mb || (ma == mb && a->gmtoff < b->gmtoff);
}

/* Append the local time type (gmtoff, dst, name) to cand[0..n) unless it is
 * there already; the new count.
 */
static int add_cand(struct tzfile_type *cand,
                    int n,
                    const struct tzfile_type *t)
{
    for (int k = 0; k < n; k++) {
        if (cand[k].gmtoff == t->gmtoff && cand[k].dst == t->dst)
            return n;
    }
    cand[n] = *t;
    return n + 1;
}

/* UTC seconds of local wall time `local` (counted as if UTC) in zone file z.
 * The instant is `local` minus the offset in force at it, and that offset is
 * one of those in force within OFF_REACH of `local`. Those are taken in time
 * order, and the first that is consistent (the offset at the instant is itself)
 * wins: in a fold, the earlier reading, which is the DST one for DST. In a gap
 * none is consistent and the one nearest to it is taken, the earliest on a tie,
 * which is the offset before the transition, as for rule zones. A tm_isdst of 0
 * or 1 prefers a consistent offset of that kind, then any of it, including the
 * zone's standard or DST offset when none of that kind is in force nearby.
 */
static long long file_local_to_utc(const struct tz_zone *z,
                                   long long local,
                                   int isdst)
{
    struct tzfile_type cand[OFF_SAMPLES + OFF_TABLE + 2], at;
    struct tzfile_type tab[OFF_TABLE];
    long mismatch[OFF_SAMPLES + OFF_TABLE + 2];
    int n = 0, pick = -1;

    for (long long t = local - OFF_REACH;; t += OFF_STEP) {
        struct tzfile_type i;

        if (t > local + OFF_REACH)
            t = local + OFF_REACH;
        zone_info(z, t, &i);
        n = add_cand(cand, n, &i);
        if (t == local + OFF_REACH)
            break;
    }

    /* The samples can miss a type that is in force for a short while; the table
     * names every one of them.
     */
    for (size_t j = 0,
                nt = __tzfile_types_in(z->loaded->file, local - OFF_REACH,
                                       local + OFF_REACH, tab, OFF_TABLE);
         j < nt; j++)
        n = add_cand(cand, n, &tab[j]);

    /* An explicit tm_isdst selects an offset of that kind even where the zone
     * has none in force nearby, as for a rule zone: offer the zone's own.
     */
    if (isdst >= 0) {
        struct tzfile_type own = {-z->std_off, 0, z->std_name};

        n = add_cand(cand, n, &own);
        if (z->daylight) {
            own = (struct tzfile_type){-z->dst_off, 1, z->dst_name};
            n = add_cand(cand, n, &own);
        }
    }
    for (int k = 0; k < n; k++) {
        zone_info(z, local - cand[k].gmtoff, &at);
        mismatch[k] = at.gmtoff > cand[k].gmtoff ? at.gmtoff - cand[k].gmtoff
                                                 : cand[k].gmtoff - at.gmtoff;
    }

    /* Among the offsets that fit (consistent ones first), the largest, which is
     * the earlier instant; among those that do not, the nearest to fitting and,
     * on a tie, the smallest, which is the offset before a step forward.
     * tm_isdst of 0 or 1 limits the choice to offsets of that kind if any.
     */
    for (int pass = isdst >= 0 ? 0 : 1; pass < 2 && pick < 0; pass++) {
        for (int k = 0; k < n; k++) {
            if (pass == 0 && (cand[k].dst > 0) != (isdst > 0))
                continue;
            if (pick < 0 || offset_better(&cand[k], mismatch[k], &cand[pick],
                                          mismatch[pick]))
                pick = k;
        }
    }
    return local - cand[pick].gmtoff;
}

time_t mktime(struct tm *tm)
{
    struct tm new;
    struct tz_zone z;
    struct tzfile_type info;
    long long t = __tm_to_secs(tm);

    /* One snapshot serves both lookups, even if TZ changes meanwhile. */
    tz_snapshot(&z);
    if (z.loaded) {
        t = file_local_to_utc(&z, t, tm->tm_isdst);
    } else {
        int dst;

        /* An explicit tm_isdst selects that offset, even against the zone. */
        if (tm->tm_isdst >= 0)
            dst = tm->tm_isdst > 0;
        else
            dst = zone_is_dst(&z, t, 1);
        t -= zone_gmtoff(&z, dst);
    }
    zone_info(&z, t, &info);
    if (!fill_tm(t, &info, &new))
        return (time_t) -1;
    *tm = new;
    return (time_t) t;
}

time_t timegm(struct tm *tm)
{
    static const struct tzfile_type utc = {0, 0, utc_name};
    struct tm new;
    long long t = __tm_to_secs(tm);

    if (!fill_tm(t, &utc, &new))
        return (time_t) -1;
    *tm = new;
    return (time_t) t;
}

double difftime(time_t t1, time_t t0)
{
    /* Operands of one sign cannot overflow the exact subtraction. */
    if ((t1 < 0) == (t0 < 0))
        return (double) (t1 - t0);
    return (double) t1 - (double) t0;
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
    if (!localtime_r(tp, &tm))
        return NULL;
    return asctime_r(&tm, buf);
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
