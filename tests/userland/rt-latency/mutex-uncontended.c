/*
 * mutex-uncontended - uncontended mutex lock/unlock latency.
 *
 * A single SCHED_FIFO thread pinned to one hart locks and unlocks a mutex with
 * no other thread contending, timing the lock-then-unlock pair with rdtime. Two
 * phases cover the two uncontended paths:
 *
 *   default   - a default-type mutex (NULL attr, PRIO_NONE). Its uncontended lock
 *               is a lockless userland compare-and-swap of the owner word, so it
 *               never enters the kernel. This is the common-case cost, and it is
 *               faster than the rdtime counter can resolve, so this phase times a
 *               batch of pairs per sample and divides back to a per-pair cost.
 *   ceiling   - a PTHREAD_PRIO_PROTECT mutex. Priority protection needs the
 *               kernel even when uncontended (it applies and restores the ceiling
 *               boost), so this path enters ksys_sync_mutex_lock and measures the
 *               kernel sync fast path round trip. It is timed one pair per sample
 *               so its tail is preserved.
 *
 * Absolute latencies under TCG are emulation-dependent (see
 * docs/rt-budgets.md); the durable signal is the two paths' max/p99 and that
 * the common default path stays in userland. Single-threaded, so it runs at any
 * hart count.
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
#include <sys/rtmux-core.h>
#include <sys/syspage.h>

#include "../outcome.h"

extern struct syspage_entry *_syspage_ptr;

#define PIN_CPU 0
#define PROBE_PRIO 40
#define CEILING_PRIO 50
#define WARMUP 64
#define ITERS 1000
#define HIST_BUCKETS 32
/* The default userland fast path is faster than the rdtime counter resolves, so
 * time this many lock/unlock pairs per sample and divide back to a per-pair
 * cost. The kernel ceiling path is above the counter resolution, so it uses 1.
 */
#define DEFAULT_BATCH 128

static uint64_t samples[ITERS];

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

static unsigned ilog2_u64(uint64_t v)
{
    unsigned b = 0;

    while (v > 1) {
        v >>= 1;
        b++;
    }
    return b;
}

static void print_histogram(const char *label, uint64_t rate, unsigned batch)
{
    unsigned hist[HIST_BUCKETS] = {0};

    for (int i = 0; i < ITERS; i++) {
        uint64_t ns = ticks_to_ns(samples[i], rate) / batch;

        unsigned b = ilog2_u64(ns);
        if (b >= HIST_BUCKETS)
            b = HIST_BUCKETS - 1;
        hist[b]++;
    }
    printf("mutex-uncontended: %s histogram (log2 ns bucket: count)\n", label);
    for (unsigned b = 0; b < HIST_BUCKETS; b++) {
        if (hist[b])
            printf("mutex-uncontended: %s hist 2^%u..2^%u ns: %u\n", label, b,
                   b + 1, hist[b]);
    }
}

/* Time WARMUP+ITERS samples of "batch" uncontended lock/unlock pairs each on an
 * already-initialized mutex and report the per-pair max/p99 plus a histogram.
 * batch amortizes the rdtime resolution for the sub-resolution userland path;
 * pass 1 to keep every pair's own tail.
 *
 * Returns 0, or -1 if a lock or unlock ever fails (which would make the numbers
 * meaningless).
 */
static int run_phase(const char *label, pthread_mutex_t *mtx, uint64_t rate,
                     unsigned batch)
{
    uint64_t maxns, p99ns;

    for (int i = 0; i < WARMUP + ITERS; i++) {
        uint64_t t0 = read_time();

        for (unsigned b = 0; b < batch; b++) {
            if (pthread_mutex_lock(mtx) != EOK ||
                pthread_mutex_unlock(mtx) != EOK) {
                printf("mutex-uncontended: %s FAIL lock/unlock\n", label);
                return -1;
            }
        }
        uint64_t t1 = read_time();
        if (i >= WARMUP)
            samples[i - WARMUP] = t1 > t0 ? t1 - t0 : 0;
    }
    qsort(samples, ITERS, sizeof samples[0], cmp_u64);
    maxns = ticks_to_ns(samples[ITERS - 1], rate) / batch;
    p99ns = ticks_to_ns(samples[(ITERS - 1) * 99 / 100], rate) / batch;
    printf("mutex-uncontended: %s lock/unlock max %llu ns p99 %llu ns over %d "
           "samples (batch %u)\n",
           label, (unsigned long long) maxns, (unsigned long long) p99ns, ITERS,
           batch);
    print_histogram(label, rate, batch);
    return 0;
}

static int run_default_phase(uint64_t rate)
{
    pthread_mutex_t mtx;
    int rc;

    if (pthread_mutex_init(&mtx, NULL) != EOK) {
        printf("mutex-uncontended: default FAIL mutex_init\n");
        return -1;
    }
    rc = run_phase("default", &mtx, rate, DEFAULT_BATCH);
    (void) pthread_mutex_destroy(&mtx);
    return rc;
}

static int run_ceiling_phase(uint64_t rate)
{
    pthread_mutexattr_t attr;
    pthread_mutex_t mtx;
    int rc;

    if (pthread_mutexattr_init(&attr) != EOK) {
        printf("mutex-uncontended: ceiling FAIL mutexattr_init\n");
        return -1;
    }
    rc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_PROTECT);
    if (rc == ENOTSUP) {
        printf("mutex-uncontended: ceiling SKIP PRIO_PROTECT unsupported\n");
        (void) pthread_mutexattr_destroy(&attr);
        return 0;
    }
    if (rc != EOK ||
        pthread_mutexattr_setprioceiling(&attr, CEILING_PRIO) != EOK) {
        printf("mutex-uncontended: ceiling FAIL setprotocol/ceiling\n");
        (void) pthread_mutexattr_destroy(&attr);
        return -1;
    }
    if (pthread_mutex_init(&mtx, &attr) != EOK) {
        printf("mutex-uncontended: ceiling FAIL mutex_init\n");
        (void) pthread_mutexattr_destroy(&attr);
        return -1;
    }
    rc = run_phase("ceiling", &mtx, rate, 1);
    (void) pthread_mutex_destroy(&mtx);
    (void) pthread_mutexattr_destroy(&attr);
    return rc;
}

int main(void)
{
    struct sched_param sp = {.sched_priority = PROBE_PRIO};
    uint64_t rate = cycles_per_sec();

    if (rate == 0) {
        printf("mutex-uncontended: SKIP counter rate unavailable\n");
        return TC_EXIT_SKIP;
    }
    /* Raise to a real-time priority and pin so the tight timing loop is not
     * migrated or preempted by ordinary threads mid-measurement.
     */
    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1 || pin_to_cpu(PIN_CPU) == -1) {
        printf("mutex-uncontended: FAIL set priority or pin: %s\n",
               strerror(errno));
        return EXIT_FAILURE;
    }

    printf("mutex-uncontended: FIFO%d on cpu%d, %d samples per phase\n",
           PROBE_PRIO, PIN_CPU, ITERS);
    fflush(stdout);

    if (run_default_phase(rate) == -1)
        return EXIT_FAILURE;
    if (run_ceiling_phase(rate) == -1)
        return EXIT_FAILURE;

    printf("mutex-uncontended: PASS uncontended lock/unlock measured\n");
    return EXIT_SUCCESS;
}
