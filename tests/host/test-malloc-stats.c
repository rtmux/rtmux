/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host test of the allocator counters in lib/libc/alloc/malloc-core.c, which
 * is linked in as it is and called through its __crt_* entry points, so the
 * host libc's own allocations never touch the counters. Every figure is exact:
 * a small block's size is its bucket (a power of two, at least 8, less the
 * 8-byte header) and a mapped block's size is the page-rounded mapping less its
 * 16-byte header.
 *
 * With "bench" as the argument it times malloc/free pairs instead (see the
 * host-bench-malloc target).
 */

#define _DEFAULT_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <sys/mallocstats.h>

void *__crt_malloc(size_t);
void *__crt_calloc(size_t, size_t);
void *__crt_realloc(void *, size_t);
void __crt_free(void *);
void *__crt_aligned_alloc_offset(size_t, size_t, size_t);
void __crt_mallocstats(struct __rtmux_mallocstats *);

static int failures;
static long checks;

#define CHECK(cond, ...)                                               \
    do {                                                               \
        checks++;                                                      \
        if (!(cond)) {                                                 \
            failures++;                                                \
            fprintf(stderr, "malloc-stats: FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__);                              \
            fputc('\n', stderr);                                       \
        }                                                              \
    } while (0)

static struct __rtmux_mallocstats snap(void)
{
    struct __rtmux_mallocstats s;

    __crt_mallocstats(&s);
    return s;
}

/* Usable size of a small request, as the allocator rounds it. */
static unsigned long long bucket(size_t n)
{
    size_t amt = 8;

    while (n > amt - 8)
        amt <<= 1;
    return amt - 8;
}

/* A mapped block of n bytes: the mapping less its header. */
static unsigned long long mapped(size_t n)
{
    return ((n + 16 + 4095) & ~(size_t) 4095) - 16;
}

static void test_counts(void)
{
    struct __rtmux_mallocstats a, b;
    void *p, *q, *big;

    a = snap();
    p = __crt_malloc(24);
    b = snap();
    CHECK(b.allocs == a.allocs + 1 && b.frees == a.frees &&
              b.bytes_in_use == a.bytes_in_use + bucket(24) &&
              b.failed == a.failed,
          "malloc(24) counts one allocation of %llu bytes", bucket(24));
    CHECK(b.bytes_peak >= b.bytes_in_use, "peak covers the bytes in use");

    __crt_free(p);
    a = snap();
    CHECK(
        a.frees == b.frees + 1 && a.bytes_in_use == b.bytes_in_use - bucket(24),
        "free returns the block's bytes");
    CHECK(a.bytes_peak == b.bytes_peak, "free does not lower the peak");

    /* Sizes at the bucket edges: 8 less the header is 0, 24 fills a 32 block.
     */
    a = snap();
    p = __crt_malloc(0);
    q = __crt_malloc(25);
    b = snap();
    CHECK(b.bytes_in_use == a.bytes_in_use + bucket(0) + bucket(25) &&
              b.allocs == a.allocs + 2,
          "bucket edges 0 and 25 (%llu and %llu bytes)", bucket(0), bucket(25));
    __crt_free(p);
    __crt_free(q);

    /* A mapped block. */
    a = snap();
    big = __crt_malloc(200000);
    b = snap();
    CHECK(b.allocs == a.allocs + 1 &&
              b.bytes_in_use == a.bytes_in_use + mapped(200000),
          "a 200000-byte block counts %llu bytes", mapped(200000));
    __crt_free(big);
    a = snap();
    CHECK(a.frees == b.frees + 1 &&
              a.bytes_in_use == b.bytes_in_use - mapped(200000),
          "freeing a mapped block returns its bytes");

    /* free(NULL) and a pointer that is not ours change nothing. */
    b = snap();
    __crt_free(NULL);
    a = snap();
    CHECK(!memcmp(&a, &b, sizeof a), "free(NULL) counts nothing");
}

static void test_failures(void)
{
    struct __rtmux_mallocstats a, b;

    a = snap();
    CHECK(__crt_malloc(SIZE_MAX) == NULL, "malloc(SIZE_MAX) fails");
    CHECK(__crt_calloc(SIZE_MAX, 2) == NULL, "calloc overflow fails");
    CHECK(__crt_aligned_alloc_offset(24, 8, 0) == NULL,
          "a bad alignment fails");
    CHECK(__crt_aligned_alloc_offset(8, SIZE_MAX, 0) == NULL,
          "an aligned overflow fails");
    b = snap();
    CHECK(b.failed == a.failed + 4 && b.allocs == a.allocs &&
              b.bytes_in_use == a.bytes_in_use,
          "four failures are counted and nothing else moves");
}

static void test_peak(void)
{
    struct __rtmux_mallocstats a, b, c;
    void *v[64];
    unsigned long long hold = 0;

    a = snap();
    for (int i = 0; i < 64; i++) {
        v[i] = __crt_malloc(100);
        hold += bucket(100);
    }
    b = snap();
    CHECK(b.bytes_peak >= a.bytes_in_use + hold, "peak reaches the bytes held");
    for (int i = 0; i < 64; i++)
        __crt_free(v[i]);
    c = snap();
    CHECK(c.bytes_in_use == a.bytes_in_use && c.bytes_peak == b.bytes_peak,
          "freeing everything lowers the use and not the peak");
    CHECK(c.allocs - a.allocs == 64 && c.frees - a.frees == 64,
          "64 allocations and 64 frees");
}

static void test_realloc_and_align(void)
{
    struct __rtmux_mallocstats a, b;
    void *p, *q;

    a = snap();
    p = __crt_malloc(24);
    q = __crt_realloc(p, 24); /* same block: nothing to count */
    b = snap();
    CHECK(q == p && b.allocs == a.allocs + 1 && b.frees == a.frees,
          "realloc to the same size is in place");
    p = __crt_realloc(q, 1000); /* moves: one allocation and one free */
    b = snap();
    CHECK(b.allocs == a.allocs + 2 && b.frees == a.frees + 1 &&
              b.bytes_in_use == a.bytes_in_use + bucket(1000),
          "a moving realloc is one allocation and one free");
    __crt_free(p);

    a = snap();
    p = __crt_aligned_alloc_offset(256, 100, 0);
    b = snap();
    CHECK(p && ((uintptr_t) p & 255) == 0 && b.allocs == a.allocs + 1,
          "an aligned block is one allocation");
    __crt_free(p);
    b = snap();
    CHECK(b.bytes_in_use == a.bytes_in_use && b.frees == a.frees + 1,
          "freeing an aligned block returns all its bytes");
}

#define THREADS 4
#define ROUNDS 50000

static void *hammer(void *arg)
{
    void *p[8];

    (void) arg;
    for (int i = 0; i < ROUNDS; i++) {
        for (int k = 0; k < 8; k++)
            p[k] = __crt_malloc((size_t) (8 + 40 * k));
        for (int k = 0; k < 8; k++)
            __crt_free(p[k]);
    }
    return NULL;
}

static void test_threads(void)
{
    struct __rtmux_mallocstats a, b;
    pthread_t t[THREADS];

    a = snap();
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, hammer, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    b = snap();
    CHECK(b.allocs - a.allocs == (unsigned long long) THREADS * ROUNDS * 8 &&
              b.frees - a.frees == b.allocs - a.allocs &&
              b.bytes_in_use == a.bytes_in_use,
          "threads: every allocation and free is counted, none lost");
}

static double now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

/* Nanoseconds per malloc/free pair, best of 7 runs, over mixed small sizes and
 * over mapped blocks (which are counted without the lock).
 */
static void bench(void)
{
    enum { N = 2000000, K = 16, NMAP = 20000 };
    double best = 1e9, best_map = 1e9;
    void *p[K];

    for (int run = 0; run < 7; run++) {
        double t0 = now();

        for (int i = 0; i < N / K; i++) {
            for (int k = 0; k < K; k++)
                p[k] = __crt_malloc((size_t) (1 + 37 * k));
            for (int k = 0; k < K; k++)
                __crt_free(p[k]);
        }
        t0 = now() - t0;
        if (t0 < best)
            best = t0;
        t0 = now();
        for (int i = 0; i < NMAP; i++)
            __crt_free(__crt_malloc(200000));
        t0 = now() - t0;
        if (t0 < best_map)
            best_map = t0;
    }
    printf(
        "malloc-bench: %.2f ns per small malloc+free pair, %.0f ns per mapped "
        "pair\n",
        best / N * 1e9, best_map / NMAP * 1e9);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "bench")) {
        bench();
        return 0;
    }
    test_counts();
    test_failures();
    test_peak();
    test_realloc_and_align();
    test_threads();
    printf("malloc-stats: %s (%ld checks, %d failures)\n",
           failures ? "FAIL" : "PASS", checks, failures);
    return failures ? 1 : 0;
}
