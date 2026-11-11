/*
 * tickless-idle - prove a quiet system stops taking the periodic tick, then
 * confirm a one-shot timer still wakes at its requested deadline.
 *
 * The probe does no busy work: it sleeps through a quiet window so every hart
 * goes idle. Under tickless idle the kernel reprograms each idle hart to its
 * composite deadline (or cancels the timer entirely when no owner needs a
 * wakeup), so the per-hart STIMER count the kernel prints as [tickless] lines
 * collapses from the nsec_inc heartbeat rate to near zero. After the quiet
 * window the probe arms one-shot sleeps of known length and checks each wakes
 * within budget, showing the deadline path still fires precisely once idle has
 * been sleeping without a periodic tick.
 *
 * The kernel-side [tickless] evidence is asserted by the qemu-regress harness;
 * this binary owns the one-shot deadline accuracy check and prints PASS/FAIL.
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

#define QUIET_WINDOW_NS 4000000000ULL /* 4 s: several kernel report seconds */
#define ONESHOT_NS 50000000ULL        /* 50 ms one-shot sleeps */
#define ONESHOT_ITERS 20
/* A one-shot wake may land late but never early. Budget the lateness generously
 * versus a 50 ms request so slow TCG hosts pass, while an idle hart that failed
 * to rearm (waking only on the next coarse event) would blow past it.
 */
#define ONESHOT_LATE_BUDGET_NS 20000000ULL /* 20 ms */

static int monotonic_ns(uint64_t *out)
{
    if (__clock_time(CLOCK_MONOTONIC, NULL, out) == -1) {
        fprintf(stderr, "tickless-idle: clock_time: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static int sleep_ns(uint64_t ns)
{
    struct timespec ts = {
        .tv_sec = (time_t) (ns / 1000000000ULL),
        .tv_nsec = (long) (ns % 1000000000ULL),
    };

    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR) {
            fprintf(stderr, "tickless-idle: nanosleep: %s\n", strerror(errno));
            return -1;
        }
    }
    return 0;
}

int main(void)
{
    uint64_t max_late = 0;

    printf("tickless-idle: quiet window %llu ns begins\n",
           (unsigned long long) QUIET_WINDOW_NS);
    fflush(stdout);

    /* Go fully idle so the kernel can drop the periodic tick on every hart. */
    if (sleep_ns(QUIET_WINDOW_NS) != 0)
        return EXIT_FAILURE;

    printf("tickless-idle: quiet window done, checking one-shot deadlines\n");
    fflush(stdout);

    for (int i = 0; i < ONESHOT_ITERS; i++) {
        uint64_t start, end;

        if (monotonic_ns(&start) != 0)
            return EXIT_FAILURE;
        if (sleep_ns(ONESHOT_NS) != 0)
            return EXIT_FAILURE;
        if (monotonic_ns(&end) != 0)
            return EXIT_FAILURE;

        uint64_t elapsed = end - start;
        if (elapsed < ONESHOT_NS) {
            printf("tickless-idle: FAIL one-shot woke early: %llu < %llu ns\n",
                   (unsigned long long) elapsed,
                   (unsigned long long) ONESHOT_NS);
            return EXIT_FAILURE;
        }
        uint64_t late = elapsed - ONESHOT_NS;
        if (late > max_late)
            max_late = late;
    }

    if (max_late > ONESHOT_LATE_BUDGET_NS) {
        printf("tickless-idle: FAIL one-shot late %llu ns > budget %llu ns\n",
               (unsigned long long) max_late,
               (unsigned long long) ONESHOT_LATE_BUDGET_NS);
        return EXIT_FAILURE;
    }

    printf("tickless-idle: one-shot deadlines met, max_late=%llu ns\n",
           (unsigned long long) max_late);
    printf("tickless-idle: PASS\n");
    return EXIT_SUCCESS;
}
