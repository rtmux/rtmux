/*
 * rr-fairness - two equal-priority SCHED_RR threads on one hart must get a fair
 * share of the CPU, and a higher-priority thread must still preempt them.
 *
 * RR timeslice accounting charges elapsed runtime (clock_handler scales the
 * actual elapsed nanoseconds into rr_ticks; the flat-tick RR_ADD_FULLTICK macro
 * is gone) and the running thread's remaining slice feeds the composite idle
 * deadline. This probe checks the externally visible consequence: two RR
 * workers pinned to the same non-boot hart at the same priority accumulate
 * nearly equal work counts over a fixed window, which only holds if the
 * round-robin slice is charged fairly. It then confirms a higher-priority
 * worker pinned to the same hart runs to completion while the RR workers spin,
 * so raising priority still preempts an RR thread.
 *
 * Absolute counts are TCG-emulation dependent; the signal is the ratio between
 * the two equal workers (fairness) and that the high-priority worker finishes.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/rtmux-core.h>

#define WORK_MS 500  /* fair-share measurement window */
#define RR_PRIO 20   /* both RR workers share this priority */
#define HI_PRIO 40   /* preemptor priority (FIFO) */
#define WORKER_CPU 1 /* pin everything to one non-boot hart */
/* Two equal RR threads should land within this fraction of each other over the
 * window. A whole RR slice of imbalance is a few percent of a 500 ms window, so
 * 25 percent is generous headroom over fair scheduling yet far from the 100
 * percent a starved thread would show.
 */
#define FAIR_TOLERANCE_PCT 25

static atomic_int stop_workers;
static atomic_int hi_ran;

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

static int set_prio(int policy, int priority)
{
    struct sched_param sp = {.sched_priority = priority};

    return __sched_set(0, 0, policy, &sp);
}

static void *rr_worker(void *arg)
{
    volatile uint64_t *counter = arg;

    if (set_prio(SCHED_RR, RR_PRIO) == -1 || pin_to_cpu(WORKER_CPU) == -1)
        return NULL;
    while (!atomic_load_explicit(&stop_workers, memory_order_acquire))
        (*counter)++;
    return NULL;
}

/* Higher-priority FIFO worker. Raise priority before pinning: pinning to the
 * busy WORKER_CPU at the inherited low priority would leave it stuck behind the
 * RR workers and unable to run. hi_ran is set only after pin_to_cpu returns,
 * which resumes this thread on WORKER_CPU, so reaching it proves it was
 * dispatched there, preempting the spinning RR workers.
 */
static void *hi_worker(void *arg)
{
    (void) arg;
    if (set_prio(SCHED_FIFO, HI_PRIO) == -1 || pin_to_cpu(WORKER_CPU) == -1)
        return NULL;
    atomic_store_explicit(&hi_ran, 1, memory_order_release);
    return NULL;
}

static int sleep_ms(unsigned ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000U,
        .tv_nsec = (long) (ms % 1000U) * 1000000L,
    };

    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

int main(void)
{
    volatile uint64_t c0 = 0, c1 = 0;
    pthread_t w0, w1, hi;

    /* Run the controller at a priority below the RR workers so it does not
     * steal the hart, and pin it elsewhere so the workers own WORKER_CPU.
     */
    if (pin_to_cpu(0) == -1)
        fprintf(stderr, "rr-fairness: controller pin unavailable\n");

    if (pthread_create(&w0, NULL, rr_worker, (void *) &c0) != EOK ||
        pthread_create(&w1, NULL, rr_worker, (void *) &c1) != EOK) {
        fprintf(stderr, "rr-fairness: worker create failed\n");
        return EXIT_FAILURE;
    }

    printf("rr-fairness: two RR workers on cpu%d for %d ms\n", WORKER_CPU,
           WORK_MS);
    fflush(stdout);
    if (sleep_ms(WORK_MS) != 0)
        return EXIT_FAILURE;

    /* While the RR workers still spin, a higher-priority worker must preempt
     * one of them and run to completion.
     */
    if (pthread_create(&hi, NULL, hi_worker, NULL) != EOK) {
        fprintf(stderr, "rr-fairness: hi worker create failed\n");
        return EXIT_FAILURE;
    }
    (void) thread_join(hi, NULL);

    atomic_store_explicit(&stop_workers, 1, memory_order_release);
    (void) thread_join(w0, NULL);
    (void) thread_join(w1, NULL);

    int ran = atomic_load_explicit(&hi_ran, memory_order_acquire);
    uint64_t lo = c0 < c1 ? c0 : c1;
    uint64_t hival = c0 < c1 ? c1 : c0;
    printf("rr-fairness: counts c0=%llu c1=%llu hi_ran=%d\n",
           (unsigned long long) c0, (unsigned long long) c1, ran);

    if (!ran) {
        printf("rr-fairness: FAIL higher priority thread did not preempt\n");
        return EXIT_FAILURE;
    }
    if (hival == 0 || lo == 0) {
        printf("rr-fairness: FAIL a worker got no CPU time\n");
        return EXIT_FAILURE;
    }
    /* Imbalance as a percentage of the larger count. */
    unsigned imbalance = (unsigned) (((hival - lo) * 100U) / hival);
    if (imbalance > FAIR_TOLERANCE_PCT) {
        printf("rr-fairness: FAIL imbalance %u%% > %d%% (unfair round-robin)\n",
               imbalance, FAIR_TOLERANCE_PCT);
        return EXIT_FAILURE;
    }
    printf("rr-fairness: fair share imbalance=%u%%\n", imbalance);
    printf("rr-fairness: PASS\n");
    return EXIT_SUCCESS;
}
