/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * qsort-test.c — qsort and qsort_r: bounded comparisons, memory safety, and
 * results checked for order and for holding the same elements as the input.
 *
 * Pure C over the standard headers, so tests/host/test-libc-algorithms.c builds it
 * against the RTMux qsort.c as well.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

#define BIG_N 100000

static uint64_t rng_state;

static uint32_t rng(void)
{
    /* xorshift64*: deterministic, so a failure reproduces. */
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return (uint32_t) ((rng_state * 2685821657736338717ULL) >> 32);
}

/* Reference: a stable top-down merge sort over es-byte elements. */
static void merge_sort(char *a, char *tmp, size_t n, size_t es,
                       int (*cmp)(const void *, const void *))
{
    size_t h = n / 2, i = 0, j = h, k = 0;

    if (n < 2)
        return;
    merge_sort(a, tmp, h, es, cmp);
    merge_sort(a + h * es, tmp, n - h, es, cmp);
    while (i < h && j < n) {
        if (cmp(a + j * es, a + i * es) < 0)
            memcpy(tmp + k++ * es, a + j++ * es, es);
        else
            memcpy(tmp + k++ * es, a + i++ * es, es);
    }
    while (i < h)
        memcpy(tmp + k++ * es, a + i++ * es, es);
    while (j < n)
        memcpy(tmp + k++ * es, a + j++ * es, es);
    memcpy(a, tmp, n * es);
}

static int is_sorted(const char *a, size_t n, size_t es, int (*cmp)(const void *, const void *))
{
    for (size_t i = 1; i < n; i++)
        if (cmp(a + (i - 1) * es, a + i * es) > 0)
            return 0;
    return 1;
}

static size_t cmp_bytes_es;

static int cmp_bytes(const void *a, const void *b)
{
    return memcmp(a, b, cmp_bytes_es);
}

/* The output holds exactly the input's elements, duplicates included: both
 * sides sorted on every byte by the reference sort must be identical.
 */
static int same_elements(const char *out, const char *in, size_t n, size_t es)
{
    char *x = malloc(n * es), *y = malloc(n * es), *tmp = malloc(n * es);
    int ok = 0;

    if (x && y && tmp) {
        memcpy(x, out, n * es);
        memcpy(y, in, n * es);
        cmp_bytes_es = es;
        merge_sort(x, tmp, n, es, cmp_bytes);
        merge_sort(y, tmp, n, es, cmp_bytes);
        ok = memcmp(x, y, n * es) == 0;
    }
    free(x);
    free(y);
    free(tmp);
    return ok;
}

/* Sort a copy of in with qsort, then check order and contents. */
static int sort_and_verify(const char *in, size_t n, size_t es,
                           int (*cmp)(const void *, const void *))
{
    char *a = malloc(n * es);
    int ok;

    if (!a)
        return 0;
    memcpy(a, in, n * es);
    qsort(a, n, es, cmp);
    ok = is_sorted(a, n, es, cmp) && same_elements(a, in, n, es);
    free(a);
    return ok;
}

static int cmp_int(const void *a, const void *b)
{
    int x, y;

    memcpy(&x, a, sizeof x);
    memcpy(&y, b, sizeof y);
    return (x > y) - (x < y);
}

static int cmp_strp(const void *a, const void *b)
{
    return strcmp(*(char *const *) a, *(char *const *) b);
}

/* 3-byte records keyed on the first byte only, so duplicate keys with
 * different payloads exercise the unstable path.
 */
static int cmp_rec3(const void *a, const void *b)
{
    return (int) *(const unsigned char *) a - (int) *(const unsigned char *) b;
}

/* McIlroy, "A Killer Adversary for Quicksort" (1999): values are assigned
 * lazily so that each pivot turns out to be nearly the smallest element.
 */
static int *anti_val;
static size_t anti_gas, anti_nsolid, anti_candidate;
static unsigned long anti_ncmp;

static int cmp_anti(const void *pa, const void *pb)
{
    size_t x, y;

    memcpy(&x, pa, sizeof x);
    memcpy(&y, pb, sizeof y);
    anti_ncmp++;
    if ((size_t) anti_val[x] == anti_gas && (size_t) anti_val[y] == anti_gas)
        anti_val[x == anti_candidate ? x : y] = (int) anti_nsolid++;
    if ((size_t) anti_val[x] == anti_gas)
        anti_candidate = x;
    else if ((size_t) anti_val[y] == anti_gas)
        anti_candidate = y;
    return (anti_val[x] > anti_val[y]) - (anti_val[x] < anti_val[y]);
}

static void test_antiqsort(void)
{
    size_t n = BIG_N;
    size_t *idx = malloc(n * sizeof *idx);
    unsigned long log2n = 0, bound = 0;
    int sorted;

    anti_val = malloc(n * sizeof *anti_val);
    sorted = idx && anti_val;
    if (!sorted)
        goto report;
    for (size_t i = 0; i < n; i++) {
        idx[i] = i;
        anti_val[i] = (int) (n - 1);
    }
    anti_gas = n - 1;
    anti_nsolid = 0;
    anti_candidate = 0;
    anti_ncmp = 0;
    qsort(idx, n, sizeof *idx, cmp_anti);
    /* floor(log2(n)), 16 for 1e5: stricter than the real 16.6. */
    for (size_t m = n; m > 1; m >>= 1)
        log2n++;
    bound = 3UL * n * log2n;
    for (size_t i = 1; i < n; i++)
        if (anti_val[idx[i - 1]] > anti_val[idx[i]])
            sorted = 0;
    printf("  INFO  antiqsort n=%zu: %lu comparisons, bound %lu\n", n, anti_ncmp, bound);
report:
    check(sorted && anti_ncmp <= bound,
          "qsort antiqsort n=1e5 sorted within 3*n*log2(n) comparisons",
          "out of order, over the comparison bound, or no memory");
    free(idx);
    free(anti_val);
}

static unsigned long es0_calls;

static int cmp_count(const void *a, const void *b)
{
    (void) a;
    (void) b;
    es0_calls++;
    return 0;
}

static void test_es_zero(void)
{
    unsigned char buf[256], copy[256];

    for (size_t i = 0; i < sizeof buf; i++)
        buf[i] = (unsigned char) (i * 7 + 3);
    memcpy(copy, buf, sizeof buf);
    es0_calls = 0;
    qsort(buf + 64, 100, 0, cmp_count);
    check(memcmp(buf, copy, sizeof buf) == 0 && es0_calls == 0,
          "qsort es==0 n=100 writes nothing", "buffer changed or comparator called");
    qsort(buf, 1, 4, cmp_count);
    qsort(buf, 0, 4, cmp_count);
    check(memcmp(buf, copy, sizeof buf) == 0 && es0_calls == 0, "qsort n<2 writes nothing",
          "buffer changed or comparator called");
}

static void test_random_ints(void)
{
    int *in = malloc(BIG_N * sizeof *in);
    int ok = in != NULL;

    if (ok) {
        for (size_t i = 0; i < BIG_N; i++)
            in[i] = (int) rng();
        ok = sort_and_verify((const char *) in, BIG_N, sizeof *in, cmp_int);
    }
    if (ok) {
        for (size_t i = 0; i < BIG_N; i++)
            in[i] = (int) (rng() % 16); /* many duplicate keys */
        ok = sort_and_verify((const char *) in, BIG_N, sizeof *in, cmp_int);
    }
    check(ok, "qsort 1e5 random ints", "output unsorted, elements differ, or no memory");
    free(in);
}

static void test_random_strings(void)
{
    enum { LEN = 12 };
    char *pool = malloc((size_t) BIG_N * LEN);
    char **in = malloc(BIG_N * sizeof *in);
    int ok = 0;

    if (pool && in) {
        for (size_t i = 0; i < BIG_N; i++) {
            char *s = pool + i * LEN;
            size_t len = 1 + rng() % (LEN - 1);

            for (size_t j = 0; j < len; j++)
                s[j] = (char) ('a' + rng() % 4); /* small alphabet: shared prefixes */
            s[len] = '\0';
            in[i] = s;
        }
        ok = sort_and_verify((const char *) in, BIG_N, sizeof *in, cmp_strp);
    }
    check(ok, "qsort 1e5 random strings", "output unsorted, elements differ, or no memory");
    free(pool);
    free(in);
}

static void test_records3(void)
{
    unsigned char *in = malloc((size_t) BIG_N * 3);
    int ok = 0;

    if (in) {
        for (size_t i = 0; i < (size_t) BIG_N * 3; i++)
            in[i] = (unsigned char) rng();
        ok = sort_and_verify((const char *) in, BIG_N, 3, cmp_rec3);
    }
    check(ok, "qsort 1e5 3-byte records", "output unsorted, elements differ, or no memory");
    free(in);
}

/* Patterns that defeat naive pivots, at sizes around the insertion and ninther
 * cut-offs.
 */
static void test_patterns(void)
{
    static const size_t sizes[] = {2, 3, 15, 16, 17, 40, 41, 100, 1000, 4097};
    int *in = malloc(4097 * sizeof *in);
    int ok = in != NULL;

    for (size_t s = 0; ok && s < sizeof sizes / sizeof sizes[0]; s++) {
        size_t n = sizes[s];

        for (int pat = 0; ok && pat < 6; pat++) {
            for (size_t i = 0; i < n; i++) {
                switch (pat) {
                case 0: in[i] = (int) i; break;                        /* sorted */
                case 1: in[i] = (int) (n - i); break;                  /* reverse */
                case 2: in[i] = 5; break;                              /* all equal */
                case 3: in[i] = (int) (i < n / 2 ? i : n - i); break;  /* organ pipe */
                case 4: in[i] = (int) (i % 3); break;                  /* few keys */
                default: in[i] = (int) (rng() % n); break;
                }
            }
            ok = sort_and_verify((const char *) in, n, sizeof *in, cmp_int);
        }
    }
    check(ok, "qsort sorted/reverse/equal/organ-pipe/few-keys patterns",
          "output unsorted or elements differ");
    free(in);
}

/* Element sizes 1..33 at every byte offset within a word, so both the word and
 * the byte swap paths run, with guard bytes on either side that must survive.
 */
static void test_alignment(void)
{
    enum { N = 57, MAXES = 33, GUARD = 16 };
    size_t cap = GUARD + 8 + (size_t) N * MAXES + GUARD;
    unsigned char *buf = malloc(cap), *in = malloc((size_t) N * MAXES);
    int ok = buf && in;

    for (size_t es = 1; ok && es <= MAXES; es++) {
        for (size_t off = 0; ok && off < 8; off++) {
            unsigned char *a = buf + GUARD + off;

            memset(buf, 0xa5, cap);
            for (size_t i = 0; i < N * es; i++)
                in[i] = (unsigned char) (rng() % 8);
            memcpy(a, in, N * es);
            cmp_bytes_es = es;
            qsort(a, N, es, cmp_bytes);
            ok = is_sorted((const char *) a, N, es, cmp_bytes) &&
                 same_elements((const char *) a, (const char *) in, N, es);
            for (size_t i = 0; ok && i < GUARD + off; i++)
                ok = buf[i] == 0xa5;
            for (size_t i = GUARD + off + N * es; ok && i < cap; i++)
                ok = buf[i] == 0xa5;
        }
    }
    check(ok, "qsort element sizes 1..33 at every alignment",
          "unsorted, elements differ, or guard bytes overwritten");
    free(buf);
    free(in);
}

static int cmp_random(const void *a, const void *b)
{
    (void) a;
    (void) b;
    return (int) (rng() % 3) - 1;
}

/* A comparator that answers at random is not an order, so the result is
 * unspecified, but the sort must still terminate, stay inside the array, and
 * only permute it.
 */
static void test_inconsistent(void)
{
    enum { N = 5000, GUARD = 16 };
    int *buf = malloc((N + 2 * GUARD) * sizeof *buf), *in = malloc(N * sizeof *in);
    int ok = buf && in;

    if (ok) {
        for (size_t i = 0; i < N + 2 * GUARD; i++)
            buf[i] = -1;
        for (size_t i = 0; i < N; i++)
            in[i] = (int) i;
        memcpy(buf + GUARD, in, N * sizeof *in);
        qsort(buf + GUARD, N, sizeof *buf, cmp_random);
        ok = same_elements((const char *) (buf + GUARD), (const char *) in, N, sizeof *in);
        for (size_t i = 0; ok && i < GUARD; i++)
            ok = buf[i] == -1 && buf[GUARD + N + i] == -1;
    }
    check(ok, "qsort with an inconsistent comparator only permutes the array",
          "elements lost or guard words overwritten");
    free(buf);
    free(in);
}

struct r_ctx {
    unsigned long calls;
    int descending;
};

static int cmp_int_r(const void *a, const void *b, void *arg)
{
    struct r_ctx *c = arg;
    int r = cmp_int(a, b);

    c->calls++;
    return c->descending ? -r : r;
}

static void test_qsort_r(void)
{
    enum { N = 1000 };
    int v[N], in[N];
    struct r_ctx c = {0, 1};
    int ok = 1;

    for (size_t i = 0; i < N; i++)
        in[i] = (int) (rng() % 500);
    memcpy(v, in, sizeof v);
    qsort_r(v, N, sizeof v[0], cmp_int_r, &c);
    for (size_t i = 1; i < N; i++)
        if (v[i - 1] < v[i])
            ok = 0;
    ok = ok && same_elements((const char *) v, (const char *) in, N, sizeof v[0]);
    check(ok && c.calls > 0, "qsort_r passes its context through",
          "context not delivered, order wrong, or elements differ");
}

void test_qsort(void)
{
    rng_state = 0x9e3779b97f4a7c15ULL;
    test_es_zero();
    test_qsort_r();
    test_alignment();
    test_inconsistent();
    test_patterns();
    test_records3();
    test_random_ints();
    test_random_strings();
    test_antiqsort();
}
