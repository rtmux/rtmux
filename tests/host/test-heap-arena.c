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

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "heap-stats.h"
#include "malloc-core.h"

/* The arena heap.c is built with. */
#define HEAP_ARENA CONFIG_ROOTTASK_HEAP_SIZE

void *__crt_morecore(size_t);
void __crt_lesscore(void *, size_t);

/* A depleted PI turnstile pool rejects the first lock attempt. */
static _Thread_local int lock_attempt;
int __real_pthread_mutex_lock(pthread_mutex_t *mutex);
int __wrap_pthread_mutex_lock(pthread_mutex_t *mutex)
{
    if (lock_attempt && ++lock_attempt == 2)
        return EAGAIN;
    return __real_pthread_mutex_lock(mutex);
}

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

/* Take every free extent and the bump region, so that no request can get new
 * backing. Returns how many blocks were taken.
 */
static int drain(struct blk *held, int max)
{
    struct roottask_heap_stats st;
    int n = 0;

    for (; n < max; n++) {
        roottask_heap_stats(&st);
        if (!st.largest)
            break;
        held[n].n = st.largest;
        if (!(held[n].p = __crt_morecore(st.largest)))
            break;
    }
    return n;
}

static unsigned long long failed_requests(void)
{
    struct __rtmux_mallocstats ms;

    __crt_mallocstats(&ms);
    return ms.failed;
}

/* The largest request served from a size class is the one that fills a 32 KiB
 * block with its header word. One byte more, up to 32767, used to take a 64
 * KiB class block that is never given back; it is mapped now, and returns its
 * pages when freed.
 */
static void test_boundary(void)
{
    struct roottask_heap_stats before, after;
    unsigned char *p;

    CHECK((p = __crt_malloc(32760)) != NULL, "32760 byte allocation");
    __crt_free(p); /* leaves a chunk behind for the size classes */
    roottask_heap_stats(&before);
    CHECK(before.used > 0, "a 32760 byte request is cut from a size class");
    for (size_t n = 32761; n <= 32768; n++) {
        CHECK((p = __crt_malloc(n)) != NULL, "%zu byte allocation", n);
        if (!p)
            continue;
        memset(p, 0x5a, n);
        roottask_heap_stats(&after);
        CHECK(after.used == before.used + 36864,
              "a %zu byte request maps 9 pages: used %zu to %zu", n,
              before.used, after.used);
        __crt_free(p);
        roottask_heap_stats(&after);
        CHECK(after.used == before.used && after.extents == before.extents,
              "a freed %zu byte block returns its backing: used %zu to %zu", n,
              before.used, after.used);
    }
}

/* With no backing left, a request that would be mapped is served from memory
 * the size classes already hold: the unused tail of their chunk, then a freed
 * block of its class, then a larger free block cut down. With none of those
 * it fails, and the failure is counted once.
 */
static void test_large_cache(void)
{
    static struct blk held[64];
    struct blk a, b, c;
    unsigned long long failed;
    void *small = __crt_malloc(8); /* the classes hold a chunk from here on */
    int nheld = drain(held, 64);
    struct roottask_heap_stats st;

    roottask_heap_stats(&st);
    CHECK(small && st.largest == 0, "arena drained: largest %zu", st.largest);

    /* a and b come from the tail of the chunk; b is freed and taken again. */
    failed = failed_requests();
    if (take(&a, 1, 70000, 5) != 1 || take(&b, 1, 40000, 6) != 1 ||
        give(&b, 1, 6) != 0 || take(&b, 1, 40000, 7) != 1) {
        CHECK(0, "requests of 70000 and 40000 bytes from cached memory");
        goto out;
    }
    CHECK(give(&a, 1, 5) == 0, "the 70000 byte block keeps its contents");
    /* The tail is under 64 KiB now and the 64 KiB class is empty: this one is
     * half of the freed 128 KiB block.
     */
    CHECK(take(&c, 1, 40000, 8) == 1, "40000 bytes cut from a 128 KiB block");
    CHECK(failed_requests() == failed, "no request failed so far");
    CHECK(__crt_malloc(70000) == NULL, "70000 bytes with no 128 KiB left");
    CHECK(failed_requests() == failed + 1, "the failure is counted once");
    roottask_heap_stats(&st);
    CHECK(st.largest == 0, "no backing was taken: largest %zu", st.largest);
    CHECK(give(&b, 1, 7) == 0, "the 40000 byte block keeps its contents");
    if (c.p)
        CHECK(give(&c, 1, 8) == 0, "the cut block keeps its contents");

out:
    while (nheld--)
        __crt_lesscore(held[nheld].p, held[nheld].n);
    __crt_free(small);
}

/* The extent table has one slot for every two pages of the arena and no check
 * for a full table (see FREE_EXTENTS in heap.c): free every other page of a
 * fresh arena, which is the most extents any sequence can leave, and give
 * the rest back.
 */
#define HEAP_PAGE 4096
static void *limit_pg[HEAP_ARENA / HEAP_PAGE];

static void test_extent_limit(void)
{
    struct roottask_heap_stats st;
    unsigned n = HEAP_ARENA / HEAP_PAGE, got = 0;

    roottask_heap_stats(&st);
    CHECK(st.used == 0 && st.extents == 0, "fresh arena: %zu used", st.used);
    while (got < n && (limit_pg[got] = __crt_morecore(HEAP_PAGE)))
        got++;
    CHECK(got == n, "%u single pages: got %u", n, got);
    for (unsigned i = 0; i + 1 < got; i += 2)
        __crt_lesscore(limit_pg[i], HEAP_PAGE);
    roottask_heap_stats(&st);
    CHECK(st.extents == n / 2 && st.extent_bytes == (size_t) n / 2 * HEAP_PAGE,
          "every other page free: %u extents, %zu bytes", st.extents,
          st.extent_bytes);
    CHECK(st.largest == HEAP_PAGE, "no two free pages touch: largest %zu",
          st.largest);
    for (unsigned i = 1; i < got; i += 2)
        __crt_lesscore(limit_pg[i], HEAP_PAGE);
    if (got & 1)
        __crt_lesscore(limit_pg[got - 1], HEAP_PAGE);
    roottask_heap_stats(&st);
    CHECK(st.extents == 0 && st.used == 0,
          "all pages returned: %u extents, used %zu", st.extents, st.used);
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
    CHECK(st.extents == 30,
          "30 gaps: %u extents", st.extents);
    for (int i = 1; i < n; i += 2)
        __crt_free(merge_b[i].p);
    for (int i = 60; i < n; i += 2)
        __crt_free(merge_b[i].p);
    roottask_heap_stats(&st);
    CHECK(st.extents == 0 && st.used == 0,
          "all blocks freed: %u extents, used %zu", st.extents, st.used);
    CHECK(st.largest == st.arena, "the whole arena is one block: %zu",
          st.largest);

    /* The most gaps this test makes: none may be given up. */
    CHECK(take(merge_b, n, 40000, 9) == n, "%d blocks of 40000 bytes again", n);
    for (int i = 0; i < n - 1;
         i += 2) /* the last stays, so nothing is at the top */
        __crt_free(merge_b[i].p);
    roottask_heap_stats(&st);
    CHECK(st.extents == (unsigned) n / 2,
          "%d gaps: %u extents", n / 2, st.extents);
    for (int i = 1; i < n; i += 2)
        __crt_free(merge_b[i].p);
    roottask_heap_stats(&st);
    CHECK(st.extents == 0 && st.used == 0,
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

/* With no argument every test runs in turn. With "lock", "merge",
 * "extent-limit", "boundary", "large-cache", "threads", "churn" or "cross" only
 * that one runs, in a fresh arena.
 */
int main(int argc, char **argv)
{
    const char *only = argc > 1 ? argv[1] : NULL;

    if (!only || !strcmp(only, "lock")) {
        struct roottask_heap_stats st;
        lock_attempt = 1;
        roottask_heap_stats(&st);
        CHECK(lock_attempt == 3, "extent access must retry after EAGAIN");
        lock_attempt = 0;
    }
    if (!only || !strcmp(only, "merge"))
        test_merge();
    if (!only || !strcmp(only, "extent-limit"))
        test_extent_limit();
    if (!only || !strcmp(only, "boundary"))
        test_boundary();
    if (!only || !strcmp(only, "large-cache"))
        test_large_cache();
    if (!only || !strcmp(only, "threads"))
        test_threads();
    if (!only || !strcmp(only, "churn"))
        test_class_churn();
    if (!only || !strcmp(only, "cross"))
        test_cross_class();
    printf("heap-arena: %ld checks, %d failures\n", checks, failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
