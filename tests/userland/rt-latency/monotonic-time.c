/*
 * monotonic-time - prove CLOCK_MONOTONIC stays continuous and monotonic while
 * tickless idle skips the periodic tick.
 *
 * The kernel publishes qtimeptr->nsec only when the boot CPU's clock handler
 * runs. Under tickless idle the boot CPU sleeps to the composite deadline, so a
 * reader would see qtimeptr->nsec frozen for the length of a skipped-tick gap
 * if the clock path returned the published value. snap_time now reads
 * arch_timer_now_ns (the free-running cycle counter), so the read stays current
 * regardless of publication.
 *
 * Two phases:
 *   1. Continuity: pin to a non-boot hart (so the boot CPU is free to idle and
 *      stop republishing qtp->nsec), then busy-read CLOCK_MONOTONIC in a tight
 *      loop. Time must never step backward, and the longest run of identical
 *      consecutive readings must stay small. A frozen qtp->nsec would show a
 *      long identical run followed by a jump; a continuous cycle-counter read
 *      advances almost every call. The probe stays SCHED_OTHER (no priority
 *      raise) so it cannot trip the separate SMP timer-wake preemption gap that
 *      only affects a high-priority thread contending a busy hart.
 *   2. Accuracy across a real idle gap: sleep about a second, then confirm the
 *      elapsed monotonic time reflects the sleep within budget.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/rtmux-core.h>

#define READ_ITERS 100000 /* tight-loop reads */
/* A continuous clock advances on nearly every read (the cycle counter moves
 * between two syscalls), so the observed run of identical consecutive readings
 * is zero. A frozen qtp->nsec repeats the same value for every read across a
 * skipped-tick idle gap, which at this read rate is hundreds to thousands in a
 * row. 100 is far above the continuous case and far below a real freeze.
 */
#define MAX_FROZEN_RUN 100
#define SLEEP_NS 1000000000ULL /* 1 s idle gap for the accuracy phase */
/* The elapsed time is measured on this (pinned, non-boot) hart via the cycle
 * counter, while the sleep timeout fires from the boot hart's clock. Under
 * multi-hart QEMU-TCG the two harts' emulated mtime can skew by a few tens of
 * ms (they advance by instructions retired per vCPU, not a shared wall clock),
 * so the measured sleep can land a little short or long of the request. Budget
 * that skew both ways: the phase still catches a frozen clock (elapsed near
 * zero) or a grossly wrong sleep, which is its point, without tripping on TCG
 * skew.
 */
#define SLEEP_SKEW_NS 150000000ULL /* 150 ms either side */

static int monotonic_ns(uint64_t *out)
{
    if (__clock_time(CLOCK_MONOTONIC, NULL, out) == -1) {
        fprintf(stderr, "monotonic-time: clock_time: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

/* Pin this thread to the given CPU so the boot CPU (CPU 0) is left free to go
 * idle. A single-CPU boot has no non-boot hart; the caller tolerates failure.
 */
static int pin_to_cpu(unsigned cpu)
{
    unsigned size = 1U;
    size_t bytes =
        sizeof(struct _thread_runmask) + 2U * size * sizeof(unsigned);
    struct _thread_runmask *m = calloc(1U, bytes);
    unsigned *runmask;
    unsigned *inherit;
    unsigned mask = 0;
    int rc;

    if (!m)
        return -1;
    RMSK_SET(cpu, &mask);
    m->size = size;
    runmask = (unsigned *) (m + 1);
    inherit = runmask + size;
    runmask[0] = mask;
    inherit[0] = mask;
    rc = thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, m);
    free(m);
    return rc;
}

int main(void)
{
    uint64_t prev, cur, start, end;
    long backward = 0;
    long run = 0, max_run = 0;

    if (pin_to_cpu(1) == -1)
        fprintf(stderr, "monotonic-time: pin CPU1 unavailable, unpinned\n");

    printf("monotonic-time: continuity phase, %d reads\n", READ_ITERS);
    fflush(stdout);

    if (monotonic_ns(&prev) != 0)
        return EXIT_FAILURE;
    for (int i = 0; i < READ_ITERS; i++) {
        if (monotonic_ns(&cur) != 0)
            return EXIT_FAILURE;
        if (cur < prev) {
            backward++;
        } else if (cur == prev) {
            if (++run > max_run)
                max_run = run;
        } else {
            run = 0;
        }
        prev = cur;
    }

    if (backward != 0) {
        printf("monotonic-time: FAIL clock stepped backward %ld times\n",
               backward);
        return EXIT_FAILURE;
    }
    if (max_run > MAX_FROZEN_RUN) {
        printf(
            "monotonic-time: FAIL %ld identical reads in a row > %d "
            "(clock froze across an idle gap)\n",
            max_run, MAX_FROZEN_RUN);
        return EXIT_FAILURE;
    }
    printf("monotonic-time: continuity ok, max_identical_run=%ld\n", max_run);

    /* Accuracy across a real idle gap: sleep, then confirm elapsed monotonic
     * time reflects the sleep and did not stall.
     */
    if (monotonic_ns(&start) != 0)
        return EXIT_FAILURE;
    {
        struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
        while (nanosleep(&ts, &ts) != 0) {
            if (errno != EINTR) {
                fprintf(stderr, "monotonic-time: nanosleep: %s\n",
                        strerror(errno));
                return EXIT_FAILURE;
            }
        }
    }
    if (monotonic_ns(&end) != 0)
        return EXIT_FAILURE;

    uint64_t elapsed = end - start;
    if (end < start) {
        printf("monotonic-time: FAIL clock went backward across the sleep\n");
        return EXIT_FAILURE;
    }
    if (elapsed + SLEEP_SKEW_NS < SLEEP_NS ||
        elapsed > SLEEP_NS + SLEEP_SKEW_NS) {
        printf(
            "monotonic-time: FAIL sleep elapsed %llu ns not within %llu ns of "
            "requested %llu ns\n",
            (unsigned long long) elapsed, (unsigned long long) SLEEP_SKEW_NS,
            (unsigned long long) SLEEP_NS);
        return EXIT_FAILURE;
    }
    printf("monotonic-time: idle-gap accuracy ok, elapsed=%llu ns\n",
           (unsigned long long) elapsed);

    printf("monotonic-time: PASS\n");
    return EXIT_SUCCESS;
}
