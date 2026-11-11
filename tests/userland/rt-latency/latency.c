/*
 * rt-latency — cyclictest-style wakeup jitter probe.
 *
 * Periodic high-priority thread under SCHED_FIFO; the deadline is computed once
 * and advanced by `period_ns` each iteration, so scheduling overhead on
 * iteration N never leaks into iteration N+1. Each iteration sleeps for
 * `deadline - now`, not for `period_ns` — that is the userland equivalent of
 * clock_nanosleep(TIMER_ABSTIME) and keeps wake-up jitter visible instead of
 * compounding.
 *
 * Reports min/avg/max plus a logarithmic-ish histogram aligned with
 * docs/rt-budgets.md so the worst bucket is visible at a glance.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/rtmux-core.h>

#include "chrome-trace.h"

#define DEFAULT_ITERATIONS 10000
#define DEFAULT_PERIOD_NS 1000000L /* 1 ms */
#define DEFAULT_PRIORITY 60        /* well above default user prio */

/* Bucket upper bounds (ns). Last bucket catches everything bigger. */
static const long buckets_ns[] = {
    1000L,  2500L,   5000L,   10000L,  25000L,
    50000L, 100000L, 250000L, 500000L, 1000000L,
};
#define NUM_BUCKETS (sizeof buckets_ns / sizeof buckets_ns[0])

static void ns_to_ts(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec = (time_t) (ns / 1000000000ULL);
    ts->tv_nsec = (long) (ns % 1000000000ULL);
}

static int monotonic_ns(uint64_t *out)
{
    if (__clock_time(CLOCK_MONOTONIC, NULL, out) == -1) {
        fprintf(stderr, "clock_time: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int raise_to_rt(int priority)
{
    struct sched_param sp = {.sched_priority = priority};

    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1) {
        fprintf(stderr, "__sched_set(SCHED_FIFO, %d): %s\n", priority,
                strerror(errno));
        return -1;
    }
    return 0;
}

static int cmp_long(const void *a, const void *b)
{
    long av = *(const long *) a;
    long bv = *(const long *) b;

    return (av > bv) - (av < bv);
}

/* Caller validates iterations > 0; samples = 0 is not reachable. size_t math
 * avoids the signed overflow that bit "samples * 99" past ~21 M iterations.
 */
static size_t p99_index(size_t samples)
{
    return ((samples - 1) * 99) / 100;
}

int main(int argc, char **argv)
{
    int iterations = DEFAULT_ITERATIONS;
    long period_ns = DEFAULT_PERIOD_NS;
    int priority = DEFAULT_PRIORITY;
    long min_ns = 0, max_ns = 0;
    int64_t total_ns = 0;
    unsigned buckets[NUM_BUCKETS + 1] = {0};
    long *samples;
    long p99_ns;
    uint64_t start_ns;
    uint64_t deadline_ns;
    bool emit_json = false;

    if (argc > 1)
        iterations = atoi(argv[1]);
    if (argc > 2)
        period_ns = atol(argv[2]);
    if (argc > 3)
        priority = atoi(argv[3]);
    if (argc > 4 && !strcmp(argv[4], "json"))
        emit_json = true;
    if (iterations <= 0 || period_ns <= 0 || priority <= 0) {
        fprintf(stderr,
                "usage: rt-latency [iterations] [period-ns] [priority] [json]\n");
        return EXIT_FAILURE;
    }

    if (raise_to_rt(priority) != 0)
        return EXIT_FAILURE;

    samples = calloc((size_t) iterations, sizeof(*samples));
    if (!samples) {
        fprintf(stderr, "calloc: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    if (monotonic_ns(&start_ns) != 0)
        return EXIT_FAILURE;
    deadline_ns = start_ns;

    for (int i = 0; i < iterations; i++) {
        uint64_t now_ns;
        long late_ns;
        unsigned b;

        deadline_ns += (uint64_t) period_ns;

        if (monotonic_ns(&now_ns) != 0)
            return EXIT_FAILURE;
        if (now_ns < deadline_ns) {
            struct timespec wait;

            ns_to_ts(deadline_ns - now_ns, &wait);
            if (nanosleep(&wait, NULL) != 0 && errno != EINTR) {
                fprintf(stderr, "nanosleep: %s\n", strerror(errno));
                return EXIT_FAILURE;
            }
        }

        if (monotonic_ns(&now_ns) != 0)
            return EXIT_FAILURE;
        late_ns = (long) (now_ns - deadline_ns);
        if (late_ns < 0)
            late_ns = 0;

        if (i == 0 || late_ns < min_ns)
            min_ns = late_ns;
        if (i == 0 || late_ns > max_ns)
            max_ns = late_ns;
        total_ns += late_ns;
        samples[i] = late_ns;

        for (b = 0; b < NUM_BUCKETS; b++) {
            if (late_ns <= buckets_ns[b])
                break;
        }
        buckets[b]++;
    }

    /* Emit the spans after the measured loop, not inside it, so the printf
     * cannot perturb the wakeups; deadline i is start + (i+1)*period.
     */
    if (emit_json) {
        int tid = chrome_tid("rt-latency");

        for (int i = 0; i < iterations; i++)
            chrome_emit_span("rt-latency", tid,
                             start_ns + (uint64_t) (i + 1) * (uint64_t) period_ns,
                             (uint64_t) samples[i]);
    }

    qsort(samples, (size_t) iterations, sizeof(*samples), cmp_long);
    p99_ns = samples[p99_index((size_t) iterations)];

    printf(
        "rt-latency: policy=SCHED_FIFO priority=%d iterations=%d "
        "period_ns=%ld min_ns=%ld avg_ns=%ld max_ns=%ld p99_ns=%ld\n",
        priority, iterations, period_ns, min_ns, (long) (total_ns / iterations),
        max_ns, p99_ns);
    printf("rt-latency histogram:");
    for (unsigned b = 0; b < NUM_BUCKETS; b++)
        printf(" <=%ldns:%u", buckets_ns[b], buckets[b]);
    printf(" >%ldns:%u\n", buckets_ns[NUM_BUCKETS - 1], buckets[NUM_BUCKETS]);
    free(samples);
    return EXIT_SUCCESS;
}
