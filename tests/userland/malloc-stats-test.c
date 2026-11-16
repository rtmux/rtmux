/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * malloc-stats-test.c - the allocator counters read through
 * __rtmux_mallocstats: allocations, frees, bytes in use, peak and failures.
 *
 * The sizes are exact for this allocator: a small block is a power-of-two
 * bucket less its 8-byte header, and a mapped block (128 KiB and up) is the
 * page-rounded mapping less 16 bytes. tests/host/test-malloc-stats.c runs the
 * same checks against malloc-core.c on the host, and adds threads.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test.h"

static struct __rtmux_mallocstats snap(void)
{
    struct __rtmux_mallocstats s;

    __rtmux_mallocstats(&s);
    return s;
}

/* Usable size of a small request. */
static unsigned long long bucket(size_t n)
{
    size_t amt = 8;

    while (n > amt - 8)
        amt <<= 1;
    return amt - 8;
}

void test_malloc_stats(void)
{
    struct __rtmux_mallocstats a, b, c;
    void *p, *q, *v[32];
    unsigned long long hold = 0;

    check(__rtmux_mallocstats(NULL) == -1 && errno == EINVAL,
          "__rtmux_mallocstats(NULL) fails with EINVAL", "no error for NULL");

    a = snap();
    p = malloc(24);
    b = snap();
    check(b.allocs == a.allocs + 1 && b.frees == a.frees &&
              b.bytes_in_use == a.bytes_in_use + bucket(24) && b.failed == a.failed,
          "malloc(24) counts one allocation of its bucket", "wrong counters after malloc");
    free(p);
    c = snap();
    check(c.frees == b.frees + 1 && c.bytes_in_use == a.bytes_in_use &&
              c.bytes_peak >= b.bytes_in_use,
          "free returns the bytes and keeps the peak", "wrong counters after free");

    a = snap();
    p = calloc(10, 10);
    q = realloc(p, 1000);
    b = snap();
    check(p && q && b.allocs == a.allocs + 2 && b.frees == a.frees + 1 &&
              b.bytes_in_use == a.bytes_in_use + bucket(1000),
          "calloc then a moving realloc: two allocations and one free", "wrong counters");
    free(q);

    a = snap();
    for (int i = 0; i < 32; i++) {
        v[i] = malloc(100);
        hold += bucket(100);
    }
    b = snap();
    for (int i = 0; i < 32; i++)
        free(v[i]);
    c = snap();
    check(b.bytes_peak >= a.bytes_in_use + hold && c.bytes_in_use == a.bytes_in_use &&
              c.bytes_peak == b.bytes_peak && c.allocs - a.allocs == 32 &&
              c.frees - a.frees == 32,
          "32 blocks held then freed: peak stays, use returns", "wrong counters");

    a = snap();
    p = malloc(300 * 1024);
    b = snap();
    free(p);
    c = snap();
    check(p && b.allocs == a.allocs + 1 && b.bytes_in_use >= a.bytes_in_use + 300 * 1024 &&
              c.bytes_in_use == a.bytes_in_use && c.frees == a.frees + 1,
          "a mapped block is counted and returned", "wrong counters for a large block");

    a = snap();
    errno = 0;
    p = malloc(SIZE_MAX);
    b = snap();
    check(p == NULL && errno == ENOMEM && b.failed == a.failed + 1 && b.allocs == a.allocs &&
              b.bytes_in_use == a.bytes_in_use,
          "a failed malloc sets ENOMEM, is counted and changes nothing else",
          "wrong counters or errno after failure");

    errno = 0;
    p = calloc(SIZE_MAX / 2, 4);
    check(p == NULL && errno == ENOMEM, "a calloc whose size overflows sets ENOMEM",
          "no ENOMEM from calloc");
    q = malloc(16);
    errno = 0;
    p = realloc(q, SIZE_MAX);
    check(p == NULL && errno == ENOMEM, "a failed realloc sets ENOMEM", "no ENOMEM from realloc");
    free(q);

    a = snap();
    free(NULL);
    b = snap();
    check(!memcmp(&a, &b, sizeof a), "free(NULL) counts nothing", "counters moved");
}
