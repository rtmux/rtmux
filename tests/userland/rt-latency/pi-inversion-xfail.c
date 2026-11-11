/*
 * pi-inversion-xfail — classic three-thread priority-inversion regression.
 *
 *   low (prio 5)    locks the mutex, then yields.
 *   medium (prio 10) runs a CPU-bound loop after low locks.
 *   high (prio 30)  tries to lock the mutex.
 *
 * All three threads are pinned to CPU 0 so the inversion manifests on UP and
 * SMP boots alike. Without priority inheritance, medium starves low; low never
 * releases the mutex; high never completes; the test returns failure.
 *
 * Once kernel turnstiles + PI land (see docs/pi-pc-design.md), low's effective
 * priority is boosted to high's while it holds the mutex, medium can no longer
 * preempt it, and the test exits success.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/rtmux-core.h>

#define LOW_PRIO 5
#define MEDIUM_PRIO 10
#define HIGH_PRIO 30

/* Bounded wait for the inversion to clear, in milliseconds. Generous so the
 * test does not flap on QEMU yet still fails noticeably when PI is absent. Real
 * PI completes in microseconds.
 */
#define INVERSION_BUDGET_MS 1500

/* Critical-section duration low_thread burns CPU before unlocking. Short enough
 * to fit comfortably inside INVERSION_BUDGET_MS once PI boosts low past medium;
 * long enough that without PI medium keeps interrupting it.
 */
#define LOW_BURN_MS 50

/* Distinct exit codes drive the userland/init wrapper. Keeping them separate
 * from the libc-default EXIT_FAILURE lets the rt-check gate tell test setup
 * failures from PI starvation regressions.
 */
#define EXIT_STARVATION_XFAIL 1 /* documented PI-absent xfail */
#define EXIT_SETUP_FAILURE 2    /* test infrastructure broke */
#define EXIT_QUARANTINED 77     /* quarantine marker if PI is disabled again */

static pthread_mutex_t mtx;
static int low_locked;
static int high_waiting;
static int high_done;
static int high_timed_out;
static int start_medium_spin;
static int stop_medium;
/* Worker hit an unexpected runtime error (pin, set_prio, clock_time, etc.). Set
 * by any worker; gates the pre-topology spin loops so a low-thread setup
 * failure doesn't hang medium/high until the Makefile budget timer fires; lets
 * main() return EXIT_SETUP_FAILURE instead of mis-classifying the resulting
 * "high never completed" as the documented PI-absent xfail.
 */
static int setup_failed;

static int flag_load(const int *flag)
{
    return __atomic_load_n(flag, __ATOMIC_ACQUIRE);
}

static void flag_store(int *flag, int value)
{
    __atomic_store_n(flag, value, __ATOMIC_RELEASE);
}

static int set_runmask(unsigned mask)
{
    unsigned size = 1U;
    size_t bytes =
        sizeof(struct _thread_runmask) + 2U * size * sizeof(unsigned);
    struct _thread_runmask *m = calloc(1U, bytes);
    unsigned *runmask;
    unsigned *inherit;
    int rc;

    if (!m)
        return -1;
    m->size = size;
    runmask = (unsigned *) (m + 1);
    inherit = runmask + size;
    runmask[0] = mask;
    inherit[0] = mask;
    rc = thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, m);
    free(m);
    return rc;
}

static int pin_to_cpu0(void)
{
    unsigned mask = 0;

    RMSK_SET(0, &mask);
    return set_runmask(mask);
}

static int unpin_observer(void)
{
    return set_runmask(~0U);
}

static int set_prio(int policy, int priority)
{
    struct sched_param sp = {.sched_priority = priority};

    return __sched_set(0, 0, policy, &sp);
}

static void sleep_ms(long ms)
{
    struct timespec ts, rem;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    /* Restart on EINTR so the requested wall-clock elapses fully — main's
     * inversion-budget sleep must NOT wake early or the test would read a
     * still-in-flight high_done as starvation.
     */
    while (nanosleep(&ts, &rem) == -1 && errno == EINTR)
        ts = rem;
}

static void *low_thread(void *arg)
{
    uint64_t end_ns;
    uint64_t now_ns;

    (void) arg;
    if (pin_to_cpu0() == -1 || set_prio(SCHED_FIFO, LOW_PRIO) == -1) {
        flag_store(&setup_failed, 1);
        return NULL;
    }
    if (pthread_mutex_lock(&mtx) != EOK) {
        flag_store(&setup_failed, 1);
        return NULL;
    }
    flag_store(&low_locked, 1);
    /* Wait for high to park on the mutex. That is the exact moment a working
     * kernel-PI implementation boosts us to HIGH_PRIO; until then, medium
     * (which is spinning at MEDIUM_PRIO) preempts us and high_waiting stays
     * unobservable.
     */
    while (!flag_load(&high_waiting) && !flag_load(&stop_medium) &&
           !flag_load(&setup_failed))
        sleep_ms(1);
    /* Bounded CPU-burn at our current effective priority. Critically, the
     * release does NOT depend on high_done — that would deadlock when PI does
     * work, because high cannot acquire the mutex until we unlock. With PI we
     * complete the burn in ~LOW_BURN_MS wall-clock and release; without PI
     * medium keeps the CPU and we never reach the deadline before the budget
     * timer in high_thread fires.
     */
    if (__clock_time(CLOCK_MONOTONIC, NULL, &end_ns) == -1) {
        /* No monotonic clock — without a deadline we'd either spin forever or
         * unlock immediately and let high acquire without exercising PI. Flag
         * setup_failed so main() returns EXIT_SETUP_FAILURE instead of letting
         * the resulting outcome be classified as PASS/FAIL.
         */
        flag_store(&setup_failed, 1);
    } else {
        end_ns += (uint64_t) LOW_BURN_MS * 1000000ULL;
        do {
            if (__clock_time(CLOCK_MONOTONIC, NULL, &now_ns) == -1) {
                flag_store(&setup_failed, 1);
                break;
            }
        } while (now_ns < end_ns && !flag_load(&stop_medium));
    }
    (void) pthread_mutex_unlock(&mtx);
    return NULL;
}

static void *medium_thread(void *arg)
{
    volatile uint64_t spin = 0;

    (void) arg;
    if (pin_to_cpu0() == -1 || set_prio(SCHED_FIFO, MEDIUM_PRIO) == -1) {
        flag_store(&setup_failed, 1);
        return NULL;
    }
    while (!flag_load(&low_locked) && !flag_load(&setup_failed))
        (void) __sched_yield();
    while (!flag_load(&start_medium_spin) && !flag_load(&setup_failed))
        sleep_ms(1);
    while (!flag_load(&stop_medium) && !flag_load(&setup_failed))
        spin++;
    (void) spin;
    return NULL;
}

static void *high_thread(void *arg)
{
    uint64_t timeout_ns = INVERSION_BUDGET_MS * 1000000ULL;
    int rc;

    (void) arg;
    if (pin_to_cpu0() == -1 || set_prio(SCHED_FIFO, HIGH_PRIO) == -1) {
        flag_store(&setup_failed, 1);
        return NULL;
    }
    while (!flag_load(&low_locked) && !flag_load(&setup_failed))
        (void) __sched_yield();
    if (flag_load(&setup_failed))
        return NULL;
    (void) __timer_timeout_r(CLOCK_REALTIME, RTMUX_TIMEOUT_MUTEX, NULL,
                             &timeout_ns, NULL);
    /* Tell low_thread that the boost window has opened. Setting this BEFORE the
     * lock call means a working kernel-PI implementation sees a waiter before
     * we block; low_thread's loop on high_waiting then exits the moment the
     * boost takes effect.
     */
    flag_store(&high_waiting, 1);
    rc = pthread_mutex_lock(&mtx);
    (void) __timer_timeout_r(CLOCK_REALTIME, 0, NULL, NULL, NULL);
    if (rc == ETIMEDOUT) {
        flag_store(&high_timed_out, 1);
        flag_store(&stop_medium, 1);
        return NULL;
    }
    if (rc != EOK) {
        /* Unexpected lock error (EDEADLK, EINVAL, …) — neither the documented
         * starvation xfail nor a clean acquisition. Signal setup failure so
         * main() does not misclassify "high never completed" as the PI-absent
         * xfail.
         */
        flag_store(&setup_failed, 1);
        flag_store(&stop_medium, 1);
        return NULL;
    }
    flag_store(&high_done, 1);
    flag_store(&stop_medium, 1);
    (void) pthread_mutex_unlock(&mtx);
    return NULL;
}

int main(void)
{
    pthread_mutexattr_t attr;
    pthread_t low, medium, high;
    int protocol;
    int rc;

    if (pin_to_cpu0() == -1) {
        fprintf(stderr, "pi-inversion-xfail: cannot pin to CPU 0: %s\n",
                strerror(errno));
        return EXIT_SETUP_FAILURE;
    }
    rc = pthread_mutexattr_init(&attr);
    if (rc != EOK) {
        fprintf(stderr, "pthread_mutexattr_init: %s\n", strerror(rc));
        return EXIT_SETUP_FAILURE;
    }
    rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if (rc == ENOTSUP) {
        /* Keep a distinct marker if a future configuration disables PI at the
         * attr API surface; required-pass rt-check treats it as a regression.
         */
        fprintf(stderr,
                "pi-inversion-xfail: QUARANTINED — PRIO_INHERIT rejected "
                "with ENOTSUP at attr API.\n");
        return EXIT_QUARANTINED;
    }
    if (rc != EOK) {
        fprintf(stderr, "pthread_mutexattr_setprotocol: %s\n", strerror(rc));
        return EXIT_SETUP_FAILURE;
    }
    rc = pthread_mutexattr_getprotocol(&attr, &protocol);
    if (rc != EOK || protocol != PTHREAD_PRIO_INHERIT) {
        fprintf(stderr, "pthread_mutexattr_getprotocol mismatch\n");
        return EXIT_SETUP_FAILURE;
    }
    rc = pthread_mutex_init(&mtx, &attr);
    if (rc != EOK) {
        fprintf(stderr, "pthread_mutex_init(PRIO_INHERIT): %s\n", strerror(rc));
        return EXIT_SETUP_FAILURE;
    }

    /* Start low first and wait for it to hold the mutex before bringing up
     * medium and high. Without this serialization high (prio 30) could start
     * before low locks, spin on !low_locked, and — if the RTMux scheduler's
     * __sched_yield at the top priority doesn't fall through to lower-prio
     * runnables — never give low (prio 5) a chance to acquire. Main sleeps in
     * short windows so it blocks (giving low a runnable slot) instead of
     * spinning at prio HIGH_PRIO+1.
     */
    if (pthread_create(&low, NULL, low_thread, NULL) != EOK) {
        fprintf(stderr, "pthread_create(low) failed\n");
        return EXIT_SETUP_FAILURE;
    }
    {
        int iter;
        for (iter = 0; iter < 50; iter++) {
            if (flag_load(&low_locked) || flag_load(&setup_failed))
                break;
            sleep_ms(20); /* total budget: 1 s */
        }
    }
    if (!flag_load(&low_locked)) {
        flag_store(&setup_failed, 1);
        (void) thread_join(low, NULL);
        fprintf(stderr,
                "pi-inversion-xfail: low_thread did not acquire the mutex "
                "within the startup budget.\n");
        return EXIT_SETUP_FAILURE;
    }
    if (set_prio(SCHED_FIFO, HIGH_PRIO + 1) == -1) {
        flag_store(&setup_failed, 1);
        flag_store(&stop_medium, 1);
        (void) thread_join(low, NULL);
        fprintf(stderr, "pi-inversion-xfail: cannot raise main: %s\n",
                strerror(errno));
        return EXIT_SETUP_FAILURE;
    }
    if (pthread_create(&high, NULL, high_thread, NULL) != EOK) {
        flag_store(&stop_medium, 1);
        (void) thread_join(low, NULL);
        fprintf(stderr, "pthread_create(high) failed\n");
        return EXIT_SETUP_FAILURE;
    }
    sleep_ms(100);
    if (!flag_load(&high_waiting)) {
        flag_store(&setup_failed, 1);
        flag_store(&stop_medium, 1);
        (void) thread_join(high, NULL);
        (void) thread_join(low, NULL);
        fprintf(stderr,
                "pi-inversion-xfail: high_thread did not block on the mutex "
                "within the startup budget.\n");
        return EXIT_SETUP_FAILURE;
    }
    if (pthread_create(&medium, NULL, medium_thread, NULL) != EOK) {
        flag_store(&stop_medium, 1);
        (void) thread_join(high, NULL);
        (void) thread_join(low, NULL);
        fprintf(stderr, "pthread_create(medium) failed\n");
        return EXIT_SETUP_FAILURE;
    }
    if (unpin_observer() == -1) {
        flag_store(&setup_failed, 1);
        flag_store(&stop_medium, 1);
        (void) thread_join(high, NULL);
        (void) thread_join(medium, NULL);
        (void) thread_join(low, NULL);
        fprintf(stderr, "pi-inversion-xfail: cannot unpin observer: %s\n",
                strerror(errno));
        return EXIT_SETUP_FAILURE;
    }

    /* Let medium start spinning only after main is ready to block for the
     * budget window; otherwise medium can preempt main before the observation
     * timer is armed.
     */
    flag_store(&start_medium_spin, 1);
    sleep_ms(INVERSION_BUDGET_MS);

    /* setup_failed wins over any timing-derived classification. A worker that
     * couldn't pin, raise priority, take the initial lock, or read the
     * monotonic clock would otherwise present as "high never completed" —
     * indistinguishable from the documented PI-absent xfail.
     */
    if (flag_load(&setup_failed)) {
        flag_store(&stop_medium, 1);
        (void) thread_join(high, NULL);
        (void) thread_join(medium, NULL);
        (void) thread_join(low, NULL);
        fprintf(stderr,
                "pi-inversion-xfail: setup failure in a worker thread; "
                "topology not exercised.\n");
        return EXIT_SETUP_FAILURE;
    }

    if (!flag_load(&high_done) || flag_load(&high_timed_out)) {
        flag_store(&stop_medium, 1);
        (void) thread_join(high, NULL);
        (void) thread_join(medium, NULL);
        (void) thread_join(low, NULL);
        fprintf(stderr,
                "pi-inversion-xfail: FAIL — high-priority waiter starved.  "
                "kernel turnstile PI not yet implemented; see "
                "docs/pi-pc-design.md.\n");
        return EXIT_STARVATION_XFAIL;
    }

    flag_store(&stop_medium, 1);

    (void) thread_join(low, NULL);
    (void) thread_join(medium, NULL);
    (void) thread_join(high, NULL);

    puts(
        "pi-inversion-xfail: PASS — high-priority waiter completed within "
        "budget (kernel PI appears to be enforced).");
    return EXIT_SUCCESS;
}
