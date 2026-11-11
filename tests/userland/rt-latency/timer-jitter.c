/*
 * timer-jitter — sweep periods under SCHED_FIFO and report wake deviation.
 *
 * Drives the same absolute-deadline loop as rt-latency, but iterates across a
 * range of periods so the operator can spot period-dependent jitter. Uses
 * `__clock_time(CLOCK_MONOTONIC)` + `nanosleep(delta)` so the measured path
 * stays aligned with rt-latency; computing the next deadline as `start + i *
 * period` keeps the cumulative drift at zero.
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

#define DEFAULT_ITERATIONS 1000
#define DEFAULT_PRIORITY 60

static const long intervals_ns[] = {
    100000L, 250000L, 500000L, 1000000L, 5000000L, 10000000L,
};
#define NUM_INTERVALS (sizeof intervals_ns / sizeof intervals_ns[0])

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
 * avoids signed overflow for large iteration counts.
 */
static size_t p99_index(size_t samples)
{
    return ((samples - 1) * 99) / 100;
}

int main(int argc, char **argv)
{
    int iterations = DEFAULT_ITERATIONS;
    int priority = DEFAULT_PRIORITY;
    long *samples;

    bool emit_json = false;

    if (argc > 1)
        iterations = atoi(argv[1]);
    if (argc > 2)
        priority = atoi(argv[2]);
    if (argc > 3 && !strcmp(argv[3], "json"))
        emit_json = true;
    if (iterations <= 0 || priority <= 0) {
        fprintf(stderr, "usage: timer-jitter [iterations] [priority] [json]\n");
        return EXIT_FAILURE;
    }

    if (raise_to_rt(priority) != 0)
        return EXIT_FAILURE;

    samples = calloc((size_t) iterations, sizeof(*samples));
    if (!samples) {
        fprintf(stderr, "calloc: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    for (unsigned i = 0; i < NUM_INTERVALS; i++) {
        long interval = intervals_ns[i];
        long min_ns = 0, max_ns = 0;
        long p99_ns;
        int64_t total_ns = 0;
        uint64_t deadline_ns;

        if (monotonic_ns(&deadline_ns) != 0)
            return EXIT_FAILURE;
        uint64_t interval_start = deadline_ns;

        for (int j = 0; j < iterations; j++) {
            uint64_t now_ns;
            long late_ns;

            deadline_ns += (uint64_t) interval;
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
            if (j == 0 || late_ns < min_ns)
                min_ns = late_ns;
            if (j == 0 || late_ns > max_ns)
                max_ns = late_ns;
            total_ns += late_ns;
            samples[j] = late_ns;
        }
        /* Post-loop emit (no in-loop printf perturbation); one track per
         * interval. Deadline j is interval_start + (j+1)*interval.
         */
        if (emit_json) {
            char name[32];

            snprintf(name, sizeof(name), "jitter-%ldus", interval / 1000);
            int tid = chrome_tid(name);

            for (int j = 0; j < iterations; j++)
                chrome_emit_span(
                    name, tid,
                    interval_start + (uint64_t) (j + 1) * (uint64_t) interval,
                    (uint64_t) samples[j]);
        }
        qsort(samples, (size_t) iterations, sizeof(*samples), cmp_long);
        p99_ns = samples[p99_index((size_t) iterations)];
        printf(
            "timer-jitter: policy=SCHED_FIFO priority=%d interval_ns=%ld "
            "iterations=%d min_ns=%ld avg_ns=%ld max_ns=%ld p99_ns=%ld\n",
            priority, interval, iterations, min_ns,
            (long) (total_ns / iterations), max_ns, p99_ns);
    }
    free(samples);
    return EXIT_SUCCESS;
}
