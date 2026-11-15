/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host test of lib/libc/posix/strftime.c, built with strftime renamed to
 * rtmux_strftime so the host libc copy is never called. It runs the shared
 * fixed vectors, and places every argument in a heap block of exactly its
 * size, so a build with AddressSanitizer traps any byte read or written
 * outside the arguments: strftime(b, 8, "ab%", tm) and strftime(b, 0, "x", tm)
 * in particular.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "strftime-cases.h"

size_t rtmux_strftime(char *s, size_t n, const char *f, const struct tm *tm);

static int failures;

static void fail(const char *what, const char *detail)
{
    failures++;
    fprintf(stderr, "strftime-test: FAIL %s: %s\n", what, detail);
}

/* A heap block of exactly n bytes (1 for n == 0), so ASan bounds it exactly:
 * a copy of src, or filled with '#' when src is NULL.
 */
static void *dup_exact(const void *src, size_t n)
{
    size_t size = n ? n : 1;
    void *p = malloc(size);

    if (!p) {
        perror("malloc");
        exit(2);
    }
    if (src)
        memcpy(p, src, n);
    else
        memset(p, '#', size);
    return p;
}

/* Format into a buffer of exactly `size` bytes, copying the result into out
 * (512 bytes) if non-NULL; return strftime's result. With size 0 the buffer is
 * a 1-byte sentinel (malloc(0) is not portable) that must stay untouched.
 */
static size_t run(const char *fmt, const struct tm *src, size_t size, char *out)
{
    char *f = dup_exact(fmt, strlen(fmt) + 1);
    struct tm *tm = dup_exact(src, sizeof *src);
    char *b = dup_exact(NULL, size);
    size_t n;

    n = rtmux_strftime(b, size, f, tm);
    if (out) {
        size_t c = size ? strnlen(b, size) : 0;

        if (c > 511)
            c = 511;
        memcpy(out, b, c);
        out[c] = '\0';
    }
    if (!size && b[0] != '#')
        fail(fmt, "maxsize 0 wrote buf[0]");
    free(b);
    free(tm);
    free(f);
    return n;
}

static void expect(const char *what,
                   const char *fmt,
                   const struct tm *tm,
                   const char *want)
{
    char got[512], detail[1200];
    size_t len = strlen(want);
    size_t n = run(fmt, tm, len + 1, got);

    if (n != len || strcmp(got, want)) {
        snprintf(detail, sizeof detail, "fmt=[%s] n=%zu got=[%s] want=[%s]",
                 fmt, n, got, want);
        fail(what, detail);
    }
    /* One byte short of the exact fit must report truncation. */
    if (len && run(fmt, tm, len, NULL) != 0)
        fail(what, "one byte short did not return 0");
}

int main(void)
{
    const struct tm *est = &strftime_vecs[STRFTIME_VEC_EST].tm;
    char what[48];
    size_t i;

    for (i = 0; i < sizeof strftime_vecs / sizeof strftime_vecs[0]; i++) {
        snprintf(what, sizeof what, "vector %zu", i);
        expect(what, STRFTIME_ALL, &strftime_vecs[i].tm, strftime_vecs[i].want);
    }
    for (i = 0; i < sizeof strftime_fmt_vecs / sizeof strftime_fmt_vecs[0];
         i++) {
        const struct strftime_fmt_vec *v = &strftime_fmt_vecs[i];

        snprintf(what, sizeof what, "format %zu", i);
        expect(what, v->fmt, &strftime_vecs[v->tm_index].tm, v->want);
    }
    for (i = 0; i < sizeof strftime_tm_vecs / sizeof strftime_tm_vecs[0]; i++) {
        snprintf(what, sizeof what, "edge %zu", i);
        expect(what, strftime_tm_vecs[i].fmt, &strftime_tm_vecs[i].tm,
               strftime_tm_vecs[i].want);
    }

    if (run("ab%", est, 8, NULL) != 0)
        fail("ab% in 8", "trailing % did not return 0");
    if (run("x", est, 0, NULL) != 0)
        fail("x in 0", "maxsize 0 did not return 0");
    if (run("%c", est, 12, NULL) != 0)
        fail("%c in 12", "truncation did not return 0");
    if (run("a%Qb", est, 32, NULL) != 0)
        fail("a%Qb", "unknown conversion did not return 0");
    if (run("%99999999999999999999Y", est, 16, NULL) != 0)
        fail("huge width", "a width past maxsize did not return 0");

    if (failures)
        return 1;
    printf("strftime-test: ok\n");
    return 0;
}
