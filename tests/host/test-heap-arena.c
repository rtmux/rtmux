/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Host test of roottask's malloc arena: the real lib/libc/alloc/malloc-core.c
 * on top of the real servers/roottask/sys/heap.c, a fixed arena that is
 * never grown. It replays what exhausted that arena in a full syscall-testing
 * run (K43): spawn buffers of one size class, then of another, taken from an
 * arena in which a size-class block is never merged or reused by another class.
 *
 * Every block is filled with a pattern of its own and checked before it is
 * freed, so overlapping blocks and a corrupt free list fail here too.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap-stats.h"

/* The arena heap.c is built with. */
#define HEAP_ARENA CONFIG_ROOTTASK_HEAP_SIZE

void *__crt_malloc(size_t);
void __crt_free(void *);

static int failures;
static long checks;

#define CHECK(cond, ...)                                             \
    do {                                                             \
        checks++;                                                    \
        if (!(cond)) {                                               \
            failures++;                                              \
            fprintf(stderr, "heap-arena: FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__);                            \
            fputc('\n', stderr);                                     \
        }                                                            \
    } while (0)

struct blk {
    unsigned char *p;
    size_t n;
};

/* Take cnt blocks of n bytes; returns how many were given, filling each. */
static int take(struct blk *b, int cnt, size_t n, unsigned char tag)
{
    int i;

    for (i = 0; i < cnt; i++) {
        b[i].n = n;
        if (!(b[i].p = __crt_malloc(n)))
            break;
        memset(b[i].p, (unsigned char) (tag + i), n);
    }
    return i;
}

/* Check the pattern of each block and free it. */
static int give(struct blk *b, int cnt, unsigned char tag)
{
    int bad = 0;

    for (int i = 0; i < cnt; i++) {
        for (size_t k = 0; k < b[i].n; k += 509)
            if (b[i].p[k] != (unsigned char) (tag + i)) {
                bad++;
                break;
            }
        __crt_free(b[i].p);
    }
    return bad;
}

/* Buffers of 36 KB (a 32 KiB argument vector and its headers), freed, then of
 * 70 KB (a 64 KiB one). As size-class blocks the first take the whole arena in
 * 64 KiB classes and the second finds no 128 KiB block; as mapped blocks that
 * are merged on free they take 5/8 of the arena and are given back whole.
 */
static struct blk churn_a[HEAP_ARENA / 65536], churn_b[HEAP_ARENA / 2 / 73728];

static void test_class_churn(void)
{
    int na = (int) (sizeof churn_a / sizeof churn_a[0]);
    int nb = (int) (sizeof churn_b / sizeof churn_b[0]);
    int n;

    n = take(churn_a, na, 36000, 1);
    CHECK(n == na, "%d blocks of 36000 bytes: got %d", na, n);
    CHECK(give(churn_a, n, 1) == 0, "36000 byte blocks keep their contents");

    n = take(churn_b, nb, 70000, 50);
    CHECK(n == nb, "%d blocks of 70000 bytes after the 36000 byte ones: got %d",
          nb, n);
    CHECK(give(churn_b, n, 50) == 0, "70000 byte blocks keep their contents");
}

/* Cached blocks of one class that is no longer asked for must serve a class
 * that is: blocks of 8000 bytes fill 3/4 of the arena and are freed, then
 * blocks of 56 bytes (64 byte class) are taken, more than the arena holds
 * without them.
 */
static struct blk cross_big[HEAP_ARENA / 8192 * 3 / 4],
    cross_small[HEAP_ARENA / 64 * 3 / 4];

static void test_cross_class(void)
{
    int nb = (int) (sizeof cross_big / sizeof cross_big[0]);
    int ns = (int) (sizeof cross_small / sizeof cross_small[0]);
    int n, m;

    n = take(cross_big, nb, 8000, 3);
    CHECK(n == nb, "%d blocks of 8000 bytes: got %d", nb, n);
    CHECK(give(cross_big, n, 3) == 0, "8000 byte blocks keep their contents");

    m = take(cross_small, ns, 56, 7);
    CHECK(m == ns, "%d blocks of 56 bytes from cached 8 KiB blocks: got %d", ns,
          m);
    CHECK(give(cross_small, m, 7) == 0, "56 byte blocks keep their contents");
}

/* Mapped blocks go back in any order and merge: nothing is left below the bump
 * pointer, and no extent is left. Then as many gaps as the arena can hold,
 * all of them kept in the extent table.
 */
static struct blk merge_b[HEAP_ARENA / 40960];

static void test_merge(void)
{
    struct roottask_heap_stats st;
    int n = (int) (sizeof merge_b / sizeof merge_b[0]);

    roottask_heap_stats(&st);
    CHECK(st.arena == HEAP_ARENA && st.used == 0, "fresh arena: %zu used",
          st.used);

    CHECK(take(merge_b, n, 40000, 9) == n, "%d blocks of 40000 bytes", n);
    for (int i = 0; i < 60; i += 2) /* 30 gaps, kept apart by live blocks */
        __crt_free(merge_b[i].p);
    roottask_heap_stats(&st);
    CHECK(st.extents == 30 && st.dropped == 0,
          "30 gaps: %u extents, %zu dropped", st.extents, st.dropped);
    for (int i = 1; i < n; i += 2)
        __crt_free(merge_b[i].p);
    for (int i = 60; i < n; i += 2)
        __crt_free(merge_b[i].p);
    roottask_heap_stats(&st);
    CHECK(st.extents == 0 && st.used == 0,
          "all blocks freed: %u extents, used %zu", st.extents, st.used);
    CHECK(st.largest == st.arena, "the whole arena is one block: %zu",
          st.largest);

    /* The most gaps this test makes: none may be given up (ext_dropped). */
    CHECK(take(merge_b, n, 40000, 9) == n, "%d blocks of 40000 bytes again", n);
    for (int i = 0; i < n - 1;
         i += 2) /* the last stays, so nothing is at the top */
        __crt_free(merge_b[i].p);
    roottask_heap_stats(&st);
    CHECK(st.extents == (unsigned) n / 2 && st.dropped == 0,
          "%d gaps: %u extents, %zu dropped", n / 2, st.extents, st.dropped);
    for (int i = 1; i < n; i += 2)
        __crt_free(merge_b[i].p);
    roottask_heap_stats(&st);
    CHECK(st.extents == 0 && st.used == 0 && st.dropped == 0,
          "everything freed again: %u extents, used %zu", st.extents, st.used);
}

/* Threads take and give mapped and size-class blocks at once: the extent table
 * and the free lists are shared, and a block handed out twice shows as a bad
 * pattern.
 */
static void *churn_thread(void *arg)
{
    unsigned tag = (unsigned) (uintptr_t) arg;
    struct blk b[3];
    long bad = 0;

    for (int round = 0; round < 3000; round++) {
        static const size_t sz[3] = {40000, 70000, 3000};
        int n = take(b, 1, sz[round % 3], (unsigned char) (tag * 16));
        int m =
            take(b + 1, 1, sz[(round + 1) % 3], (unsigned char) (tag * 16 + 1));

        bad += (n != 1) + (m != 1);
        bad += give(b, n, (unsigned char) (tag * 16));
        bad += give(b + 1, m, (unsigned char) (tag * 16 + 1));
    }
    return (void *) (uintptr_t) bad;
}

static void test_threads(void)
{
    pthread_t th[4];
    long bad = 0;

    for (unsigned i = 0; i < 4; i++)
        CHECK(pthread_create(&th[i], NULL, churn_thread,
                             (void *) (uintptr_t) i) == 0,
              "thread %u starts", i);
    for (unsigned i = 0; i < 4; i++) {
        void *r;

        CHECK(pthread_join(th[i], &r) == 0, "thread %u joins", i);
        bad += (long) (uintptr_t) r;
    }
    CHECK(bad == 0, "concurrent churn: %ld blocks with a bad pattern", bad);
}

/* With no argument every test runs in turn. With "churn", "cross", "merge" or
 * "threads" only that one runs, in a fresh arena.
 */
int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;

    if (!only || !strcmp(only, "merge"))
        test_merge();
    if (!only || !strcmp(only, "threads"))
        test_threads();
    if (!only || !strcmp(only, "churn"))
        test_class_churn();
    if (!only || !strcmp(only, "cross"))
        test_cross_class();
    printf("heap-arena: %ld checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
