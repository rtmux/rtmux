/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Reader for TZif version 2+ zone files (see tzfile.h).
 *
 * The file is read once, whole, into a single allocation bounded by TZFILE_MAX
 * and validated in full, so every later lookup indexes within the buffer
 * without checks: counts are consistent with the file size, transition times
 * strictly increase, every type index and abbreviation index is in range, every
 * abbreviation is terminated, and there are no leap second records. A file that
 * fails any check, uses version 1 only, or is missing is rejected; the caller
 * then falls back to UTC.
 *
 * The zoneinfo tree is trusted system data: names are checked lexically and
 * only a regular file is read, but a symlink inside the tree is followed.
 *
 * A lookup is a binary search over the transition times, O(log n) with n at
 * most TZFILE_MAX / 9.
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "tzfile.h"

#define HDR_LEN 44
#define NAME_MAX_LEN 100

struct tzfile {
    size_t ntrans, ntypes, nabbr;
    const unsigned char *times; /* ntrans big-endian int64 */
    const unsigned char *idx;   /* ntrans type indexes */
    const unsigned char *types; /* ntypes of utoff[4], isdst[1], abbrind[1] */
    const char *abbr;           /* nabbr bytes, the last a NUL */
    char footer[TZFILE_FOOTER_MAX + 1];
    unsigned char data[];
};

static uint32_t be32(const unsigned char *p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 |
           (uint32_t) p[2] << 8 | p[3];
}

static long long be64(const unsigned char *p)
{
    return (long long) ((uint64_t) be32(p) << 32 | be32(p + 4));
}

static void type_at(const struct tzfile *f, size_t i, struct tzfile_type *out)
{
    const unsigned char *p = f->types + 6 * i;

    out->gmtoff = (int32_t) be32(p);
    out->dst = p[4];
    out->name = f->abbr + p[5];
}

int __tzfile_name_ok(const char *name)
{
    size_t n = 0, comp = 0;

    for (; name[n]; n++) {
        unsigned char c = (unsigned char) name[n];

        if (n >= NAME_MAX_LEN)
            return 0;
        if (c == '/') {
            if (!comp)
                return 0;
            comp = 0;
            continue;
        }
        if (!isalnum(c) && c != '_' && c != '+' && c != '-' && c != '.')
            return 0;
        /* No ".", ".." or hidden component, so no escape from the directory. */
        if (!comp && c == '.')
            return 0;
        comp++;
    }
    return comp != 0;
}

/* Check the 64-bit block at d (d_len bytes available, counts from the header at
 * h) and set up f's table pointers.
 *
 * Returns the block size, or 0.
 */
static size_t parse_block(struct tzfile *f,
                          const unsigned char *h,
                          const unsigned char *d,
                          size_t d_len)
{
    uint64_t isut = be32(h + 20), isstd = be32(h + 24), leap = be32(h + 28);
    uint64_t timecnt = be32(h + 32), typecnt = be32(h + 36),
             charcnt = be32(h + 40);
    uint64_t size = timecnt * 9 + typecnt * 6 + charcnt + leap * 12 + isstd +
                    isut; /* cannot overflow: each count is below 2^32 */

    /* Transition times of a file with leap seconds are on the leap-second
     * scale, not POSIX time; such a file is rejected, not misread.
     */
    if (leap || !typecnt || !charcnt || (isstd && isstd != typecnt) ||
        (isut && isut != typecnt) || size > d_len || typecnt > 256)
        return 0;
    f->ntrans = timecnt;
    f->ntypes = typecnt;
    f->nabbr = charcnt;
    f->times = d;
    f->idx = d + timecnt * 8;
    f->types = f->idx + timecnt;
    f->abbr = (const char *) f->types + typecnt * 6;
    return (size_t) size;
}

static int validate(const struct tzfile *f)
{
    if (f->abbr[f->nabbr - 1] != '\0')
        return 0;
    for (size_t i = 0; i < f->ntypes; i++) {
        const unsigned char *p = f->types + 6 * i;
        long off = (int32_t) be32(p);

        if (off < -UTOFF_MAX || off > UTOFF_MAX || p[4] > 1 || p[5] >= f->nabbr)
            return 0;
    }
    for (size_t i = 0; i < f->ntrans; i++) {
        if (f->idx[i] >= f->ntypes ||
            (i && be64(f->times + 8 * i) <= be64(f->times + 8 * (i - 1))))
            return 0;
    }
    return 1;
}

/* Check the whole file d (len bytes) and point *f's table and footer at it.
 * Returns 0 if d is not a usable TZif version 2+ file.
 */
static int parse(struct tzfile *f, const unsigned char *d, size_t len)
{
    size_t off, size, foot_len;
    uint64_t v1_end;
    const unsigned char *end;

    /* Skip the version 1 block, then read the 64-bit block that follows. The
     * end is computed in 64 bits and checked before narrowing to size_t.
     */
    if (len < HDR_LEN || memcmp(d, "TZif", 4) || d[4] < '2')
        return 0;
    v1_end = HDR_LEN + be32(d + 32) * 5ULL + be32(d + 36) * 6ULL +
             be32(d + 40) + be32(d + 28) * 8ULL + be32(d + 24) + be32(d + 20);
    if (v1_end > len)
        return 0;
    off = (size_t) v1_end;
    if (len - off < HDR_LEN || memcmp(d + off, "TZif", 4) || d[off + 4] < '2')
        return 0;
    size = parse_block(f, d + off, d + off + HDR_LEN, len - off - HDR_LEN);
    if (!size || !validate(f))
        return 0;

    /* The footer is "\n" rule "\n"; it may be empty. */
    off += HDR_LEN + size;
    if (off >= len || d[off] != '\n')
        return 0;
    off++;
    end = memchr(d + off, '\n', len - off);
    if (!end || (size_t) (end - (d + off)) > TZFILE_FOOTER_MAX)
        return 0;
    foot_len = (size_t) (end - (d + off));
    memcpy(f->footer, d + off, foot_len);
    f->footer[foot_len] = '\0';
    return 1;
}

const struct tzfile *__tzfile_load(const char *name)
{
    char path[256];
    const char *dir = TZ_DIR_PREFIX;
    struct stat st;
    struct tzfile *f;
    size_t len, got = 0;
    int fd;

#ifdef LIBC_HOST_TEST
    /* Host tests point the reader at a fixture directory. */
    if (getenv("RTMUX_TZDIR"))
        dir = getenv("RTMUX_TZDIR");
#endif
    if (!__tzfile_name_ok(name) || strlen(dir) + strlen(name) >= sizeof path)
        return NULL;
    strcpy(path, dir);
    strcat(path, name);

    /* The open does not block on a FIFO or device, and only a regular file of a
     * sane size is read.
     */
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return NULL;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > TZFILE_MAX) {
        close(fd);
        return NULL;
    }
    len = (size_t) st.st_size;
    f = malloc(sizeof *f + len);
    while (f && got < len) {
        ssize_t n = read(fd, f->data + got, len - got);

        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t) n;
    }
    close(fd);
    if (f && (got != len || !parse(f, f->data, len))) {
        free(f);
        f = NULL;
    }
    return f;
}

const char *__tzfile_footer(const struct tzfile *f)
{
    return f->footer;
}

/* Index of the latest transition at or before t, or -1 if t precedes them all.
 */
static long find_transition(const struct tzfile *f, long long t)
{
    size_t lo = 0, hi = f->ntrans; /* the answer is in [lo - 1, hi - 1] */

    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;

        if (be64(f->times + 8 * mid) <= t)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (long) lo - 1;
}

size_t __tzfile_types_in(const struct tzfile *f,
                         long long lo,
                         long long hi,
                         struct tzfile_type *out,
                         size_t max)
{
    size_t n = 0;

    for (long i = find_transition(f, lo) + 1;
         (size_t) i < f->ntrans && n < max && be64(f->times + 8 * i) <= hi; i++)
        type_at(f, f->idx[i], &out[n++]);
    return n;
}

int __tzfile_after_table(const struct tzfile *f, long long t)
{
    return !f->ntrans || t > be64(f->times + 8 * (f->ntrans - 1));
}

void __tzfile_type_at(const struct tzfile *f,
                      long long t,
                      struct tzfile_type *out)
{
    long i = find_transition(f, t);

    type_at(f, i < 0 ? 0 : f->idx[i], out);
}

void __tzfile_last_types(const struct tzfile *f,
                         struct tzfile_type *std,
                         struct tzfile_type *dst,
                         int *has_dst)
{
    int have_std = 0, have_dst = 0;

    for (size_t i = f->ntrans; i-- > 0 && !(have_std && have_dst);) {
        struct tzfile_type t;

        type_at(f, f->idx[i], &t);
        if (t.dst && !have_dst) {
            *dst = t;
            have_dst = 1;
        } else if (!t.dst && !have_std) {
            *std = t;
            have_std = 1;
        }
    }
    if (!have_std)
        type_at(f, 0, std);
    if (!have_dst)
        *dst = *std;
    *has_dst = have_dst;
}
