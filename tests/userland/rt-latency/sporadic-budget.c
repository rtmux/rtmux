/*
 * sporadic-budget - a SCHED_SPORADIC thread must drop to its low priority when
 * its execution budget is spent and recover it after the replenishment period,
 * so a middle-priority competitor on the same hart gets the CPU in between.
 *
 * The sporadic server charges elapsed runtime against the budget in
 * clock_handler; when the budget is exhausted the thread is rescheduled to
 * ss_low_priority, and a replenishment armed one period after the run makes the
 * budget (and the high priority) available again. That next replenishment feeds
 * the composite idle deadline (CLOCK_OWNER_SS), so tickless idle wakes exactly
 * when a replenishment is due rather than scanning every tick.
 *
 * Setup: a SPORADIC worker at high priority with ss_low_priority below a
 * SCHED_RR competitor, all pinned to one non-boot hart. If the budget drop
 * works, the competitor runs while the sporadic thread is at low priority and
 * both accumulate work; if the sporadic thread never dropped it would run at
 * high priority forever and the competitor would starve.
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

#define WORK_MS 600
#define SS_HI_PRIO 30
#define SS_LO_PRIO 10
#define COMP_PRIO 20 /* between SS low and high */
#define WORKER_CPU 1
#define BUDGET_MS 20
#define PERIOD_MS 60

static atomic_int stop_workers;
/* Start gate: workers finish policy+pin, publish their ready flag, then spin
 * until main opens the gate. Without it the competitor could bank a few ticks
 * during startup before the sporadic thread is pinned and monopolizing, so a
 * broken budget drop (sporadic never yields) could still show comp > 0 and
 * false pass. Counting only after the gate opens means comp > 0 proves the
 * competitor ran while the measurement was live, i.e. the sporadic thread
 * actually dropped.
 */
static atomic_int start_gate;
static atomic_int ss_ready;
static atomic_int comp_ready;

static void wait_for_gate(void)
{
    /* Also break on stop_workers so a failing run that never opened the gate
     * can still join these threads instead of leaving them spinning.
     */
    while (!atomic_load_explicit(&start_gate, memory_order_acquire) &&
           !atomic_load_explicit(&stop_workers, memory_order_acquire))
        ;
}

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

/* Raise scheduling policy before pinning: pinning at the inherited low priority
 * onto the busy WORKER_CPU would strand the thread behind the others.
 */
static void *sporadic_worker(void *arg)
{
    volatile uint64_t *counter = arg;
    struct sched_param sp = {
        .sched_priority = SS_HI_PRIO,
        .ss_low_priority = SS_LO_PRIO,
        .ss_max_repl = 4,
        .ss_init_budget = {.tv_sec = 0, .tv_nsec = BUDGET_MS * 1000000L},
        .ss_repl_period = {.tv_sec = 0, .tv_nsec = PERIOD_MS * 1000000L},
    };

    if (__sched_set(0, 0, SCHED_SPORADIC, &sp) == -1) {
        fprintf(stderr, "sporadic-budget: SCHED_SPORADIC set: %s\n",
                strerror(errno));
        return NULL;
    }
    if (pin_to_cpu(WORKER_CPU) == -1)
        return NULL;
    atomic_store_explicit(&ss_ready, 1, memory_order_release);
    wait_for_gate();
    while (!atomic_load_explicit(&stop_workers, memory_order_acquire))
        (*counter)++;
    return NULL;
}

static void *comp_worker(void *arg)
{
    volatile uint64_t *counter = arg;
    struct sched_param sp = {.sched_priority = COMP_PRIO};

    if (__sched_set(0, 0, SCHED_RR, &sp) == -1 || pin_to_cpu(WORKER_CPU) == -1)
        return NULL;
    atomic_store_explicit(&comp_ready, 1, memory_order_release);
    wait_for_gate();
    while (!atomic_load_explicit(&stop_workers, memory_order_acquire))
        (*counter)++;
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
    volatile uint64_t ss = 0, comp = 0;
    pthread_t ws, wc;

    if (pin_to_cpu(0) == -1)
        fprintf(stderr, "sporadic-budget: controller pin unavailable\n");

    /* Create the competitor first so the higher-priority sporadic worker cannot
     * keep it from reaching the gate.
     */
    if (pthread_create(&wc, NULL, comp_worker, (void *) &comp) != EOK ||
        pthread_create(&ws, NULL, sporadic_worker, (void *) &ss) != EOK) {
        fprintf(stderr, "sporadic-budget: worker create failed\n");
        return EXIT_FAILURE;
    }

    printf(
        "sporadic-budget: SPORADIC(%d/%d budget=%dms period=%dms) vs "
        "RR(%d) on cpu%d for %d ms\n",
        SS_HI_PRIO, SS_LO_PRIO, BUDGET_MS, PERIOD_MS, COMP_PRIO, WORKER_CPU,
        WORK_MS);
    fflush(stdout);

    /* Wait until both workers finished setup, then open the gate so the
     * measurement window excludes startup ticks. Bound the wait: if the budget
     * drop is broken the sporadic thread monopolizes the hart and the
     * competitor never reaches the gate, so fail cleanly rather than hang.
     */
    for (int waited = 0;
         !atomic_load_explicit(&ss_ready, memory_order_acquire) ||
         !atomic_load_explicit(&comp_ready, memory_order_acquire);
         waited += 5) {
        if (waited >= 2000) {
            printf(
                "sporadic-budget: FAIL a worker never reached the gate "
                "(sporadic monopolized setup)\n");
            atomic_store_explicit(&stop_workers, 1, memory_order_release);
            (void) thread_join(ws, NULL);
            (void) thread_join(wc, NULL);
            return EXIT_FAILURE;
        }
        if (sleep_ms(5) != 0)
            return EXIT_FAILURE;
    }
    atomic_store_explicit(&start_gate, 1, memory_order_release);

    if (sleep_ms(WORK_MS) != 0)
        return EXIT_FAILURE;

    atomic_store_explicit(&stop_workers, 1, memory_order_release);
    (void) thread_join(ws, NULL);
    (void) thread_join(wc, NULL);

    printf("sporadic-budget: counts sporadic=%llu comp=%llu\n",
           (unsigned long long) ss, (unsigned long long) comp);

    if (ss == 0) {
        printf("sporadic-budget: FAIL sporadic worker got no CPU time\n");
        return EXIT_FAILURE;
    }
    if (comp == 0) {
        printf(
            "sporadic-budget: FAIL competitor starved (budget never "
            "dropped the sporadic thread)\n");
        return EXIT_FAILURE;
    }
    printf(
        "sporadic-budget: PASS budget drop and replenishment share the "
        "hart\n");
    return EXIT_SUCCESS;
}
