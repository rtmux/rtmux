/*
 * mutex-wake - contended PRIO_INHERIT mutex wake-to-acquire latency.
 *
 * A low-priority holder owns a PTHREAD_PRIO_INHERIT mutex on one hart; a
 * high-priority waiter on a second hart blocks on it. The measured span is from
 * the holder's unlock to the waiter returning from pthread_mutex_lock, i.e. the
 * time to wake and dispatch the blocked high-priority waiter once the lock is
 * released. That is the "contended mutex wake" row the realtime budgets track.
 *
 * The holder and waiter sit on different harts so the high-priority waiter
 * never spins on the holder's hart while it needs to run (which would starve it
 * on UP and defeat the loop). Priority inheritance is active because the mutex
 * is PRIO_INHERIT, but this probe does not depend on a boost being
 * consequential: inversion-freedom under a competing medium thread is the job
 * of pi-inversion-xfail. This probe only times the handoff.
 *
 * Each iteration is gen-sequenced so the holder always re-acquires the mutex
 * before the waiter attempts the next lock, and the holder waits a fixed guard
 * before the timed unlock so the waiter is already blocked (a released-before-
 * blocked race would measure an uncontended fast-path acquire, not a wake). The
 * guard runs before the unlock timestamp, so it never inflates a sample.
 *
 * Absolute latencies under TCG are emulation-dependent (see
 * docs/rt-budgets.md); the durable signal is that the wake completes and its
 * max/p99 over many samples, not a hardware-absolute budget. Needs at least
 * three harts, one each for the holder, the waiter, and the controller, so the
 * high-priority waiter never preempts the controller mid-setup; it skips
 * cleanly with fewer.
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
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/syspage.h>

#include "../outcome.h"

extern struct syspage_entry *_syspage_ptr;

#define HOLDER_CPU 0
#define WAITER_CPU 1
#define CTRL_CPU 2
#define HOLDER_PRIO 10
#define WAITER_PRIO 40
#define WARMUP 32
#define ITERS 200
/* Guard the holder spins, holding the mutex, after publishing the generation
 * and before the timed unlock. It only has to exceed the time for the waiter on
 * the other hart to enter pthread_mutex_lock and block; 100 us is generous and,
 * being before the unlock timestamp, costs only wall-clock.
 */
#define GUARD_NS 100000ULL
/* log2-ns histogram width: bucket k counts samples with k <= log2(ns) < k+1. */
#define HIST_BUCKETS 32

static pthread_mutex_t mtx;
static atomic_uint_least64_t gen_seq;    /* holder publishes the round number */
static atomic_uint_least64_t round_done; /* waiter echoes the finished round */
static atomic_uint_least64_t t_unlock;   /* holder rdtime just before unlock */
static atomic_int setup_failed;
static uint64_t samples[ITERS]; /* wake ticks per round, read after join */

static inline uint64_t read_time(void)
{
    uint64_t t;

    __asm__ volatile("rdtime %0" : "=r"(t));
    return t;
}

/* Counter delta to nanoseconds without a 128-bit divide (freestanding userland
 * has no __udivti3): whole seconds plus remainder keeps every multiply in
 * range.
 */
static uint64_t ticks_to_ns(uint64_t ticks, uint64_t rate)
{
    if (rate == 0)
        return ticks;
    return (ticks / rate) * 1000000000ULL +
           (ticks % rate) * 1000000000ULL / rate;
}

static uint64_t cycles_per_sec(void)
{
    if (_syspage_ptr && _syspage_ptr->qtime.entry_off)
        return SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *) a, bv = *(const uint64_t *) b;

    return (av > bv) - (av < bv);
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

/* Raise policy before pinning so the thread is not stranded at its inherited
 * priority on the target hart. A failure of either step is published so the
 * controller fails the run rather than measuring an incomplete topology.
 */
static int prio_then_pin(int priority, unsigned cpu)
{
    struct sched_param sp = {.sched_priority = priority};

    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1 || pin_to_cpu(cpu) == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return -1;
    }
    return 0;
}

static void spin_ns(uint64_t ns, uint64_t rate)
{
    uint64_t start = read_time();
    uint64_t target = (ns / 1000000000ULL) * rate +
                      (ns % 1000000000ULL) * rate / 1000000000ULL;

    while (read_time() - start < target)
        ;
}

/* Holder on HOLDER_CPU: owns the mutex, publishes each round, guards so the
 * waiter blocks, then times its own unlock. It re-acquires the mutex only after
 * the waiter echoes the finished round, so the waiter never wins the next lock.
 */
static void *holder_worker(void *arg)
{
    uint64_t rate = *(uint64_t *) arg;

    if (prio_then_pin(HOLDER_PRIO, HOLDER_CPU) == -1)
        return NULL;

    for (uint64_t i = 1; i <= WARMUP + ITERS; i++) {
        if (pthread_mutex_lock(&mtx) != EOK) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return NULL;
        }
        /* Publish the round only after the mutex is held, so the waiter that
         * proceeds on this generation blocks instead of acquiring free.
         */
        atomic_store_explicit(&gen_seq, i, memory_order_release);
        spin_ns(GUARD_NS, rate);
        atomic_store_explicit(&t_unlock, read_time(), memory_order_release);
        (void) pthread_mutex_unlock(&mtx);
        while (atomic_load_explicit(&round_done, memory_order_acquire) != i) {
            if (atomic_load_explicit(&setup_failed, memory_order_acquire))
                return NULL;
        }
    }
    return NULL;
}

/* Waiter on WAITER_CPU: blocks on the mutex each round and records the acquire
 * time. The delta from the holder's unlock timestamp to this acquire is the
 * contended wake latency.
 */
static void *waiter_worker(void *arg)
{
    uint64_t last = 0;
    (void) arg;

    if (prio_then_pin(WAITER_PRIO, WAITER_CPU) == -1)
        return NULL;

    for (;;) {
        uint64_t g, acq, rel;

        while ((g = atomic_load_explicit(&gen_seq, memory_order_acquire)) ==
               last) {
            if (atomic_load_explicit(&setup_failed, memory_order_acquire))
                return NULL;
        }
        last = g;
        if (pthread_mutex_lock(&mtx) != EOK) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return NULL;
        }
        acq = read_time();
        rel = atomic_load_explicit(&t_unlock, memory_order_acquire);
        (void) pthread_mutex_unlock(&mtx);
        if (g > WARMUP)
            samples[g - WARMUP - 1] = acq > rel ? acq - rel : 0;
        atomic_store_explicit(&round_done, g, memory_order_release);
        if (g == WARMUP + ITERS)
            return NULL;
    }
}

static unsigned ilog2_u64(uint64_t v)
{
    unsigned b = 0;

    while (v > 1) {
        v >>= 1;
        b++;
    }
    return b;
}

static void print_histogram(uint64_t rate)
{
    unsigned hist[HIST_BUCKETS] = {0};

    for (int i = 0; i < ITERS; i++) {
        uint64_t ns = ticks_to_ns(samples[i], rate);
        unsigned b = ilog2_u64(ns);

        if (b >= HIST_BUCKETS)
            b = HIST_BUCKETS - 1;
        hist[b]++;
    }
    printf("mutex-wake: histogram (log2 ns bucket: count)\n");
    for (unsigned b = 0; b < HIST_BUCKETS; b++) {
        if (hist[b])
            printf("mutex-wake: hist 2^%u..2^%u ns: %u\n", b, b + 1, hist[b]);
    }
}

int main(void)
{
    pthread_mutexattr_t attr;
    pthread_t holder, waiter;
    uint64_t rate = cycles_per_sec();
    uint64_t maxns, p99ns;
    int protocol, rc;

    if (rate == 0) {
        printf("mutex-wake: SKIP counter rate unavailable\n");
        return TC_EXIT_SKIP;
    }
    /* Sit the controller on its own hart, distinct from the holder (cpu0) and
     * waiter (cpu1) harts, so it does not compete with the steady-state
     * high-priority waiter after setup. A newly created worker briefly inherits
     * this runmask before prio_then_pin moves it to its own hart, but it leaves
     * cpu2 at once, so the controller is not stranded as it was when it pinned
     * itself to the waiter hart. A failed pin means fewer than three harts, so
     * skip.
     */
    if (pin_to_cpu(CTRL_CPU) == -1) {
        printf("mutex-wake: SKIP needs at least 3 harts\n");
        return TC_EXIT_SKIP;
    }

    rc = pthread_mutexattr_init(&attr);
    if (rc != EOK) {
        printf("mutex-wake: FAIL mutexattr_init: %s\n", strerror(rc));
        return EXIT_FAILURE;
    }
    rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if (rc != EOK) {
        printf("mutex-wake: FAIL setprotocol(PRIO_INHERIT): %s\n", strerror(rc));
        return EXIT_FAILURE;
    }
    rc = pthread_mutexattr_getprotocol(&attr, &protocol);
    if (rc != EOK || protocol != PTHREAD_PRIO_INHERIT) {
        printf("mutex-wake: FAIL getprotocol mismatch\n");
        return EXIT_FAILURE;
    }
    if (pthread_mutex_init(&mtx, &attr) != EOK) {
        printf("mutex-wake: FAIL mutex_init\n");
        return EXIT_FAILURE;
    }

    printf("mutex-wake: FIFO%d holder on cpu%d, FIFO%d waiter on cpu%d, "
           "PRIO_INHERIT, %d samples\n",
           HOLDER_PRIO, HOLDER_CPU, WAITER_PRIO, WAITER_CPU, ITERS);
    fflush(stdout);

    if (pthread_create(&waiter, NULL, waiter_worker, NULL) != EOK) {
        printf("mutex-wake: FAIL create waiter\n");
        return EXIT_FAILURE;
    }
    if (pthread_create(&holder, NULL, holder_worker, &rate) != EOK) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        (void) thread_join(waiter, NULL);
        printf("mutex-wake: FAIL create holder\n");
        return EXIT_FAILURE;
    }

    (void) thread_join(holder, NULL);
    (void) thread_join(waiter, NULL);

    if (atomic_load_explicit(&setup_failed, memory_order_acquire)) {
        printf("mutex-wake: FAIL worker setup\n");
        return EXIT_FAILURE;
    }

    /* Both workers joined, so samples is stable. Sort so max is last and p99 a
     * fixed index.
     */
    qsort(samples, ITERS, sizeof samples[0], cmp_u64);
    maxns = ticks_to_ns(samples[ITERS - 1], rate);
    p99ns = ticks_to_ns(samples[(ITERS - 1) * 99 / 100], rate);
    printf("mutex-wake: contended wake max %llu ns p99 %llu ns over %d samples\n",
           (unsigned long long) maxns, (unsigned long long) p99ns, ITERS);
    print_histogram(rate);
    printf("mutex-wake: PASS contended PRIO_INHERIT wake measured\n");
    return EXIT_SUCCESS;
}
