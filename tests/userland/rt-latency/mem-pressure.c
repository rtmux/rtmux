/*
 * mem-pressure - memory-pressure regression for the section 18 OOM policy.
 *
 * RTMux does deterministic admission control: it eagerly backs a mapping with
 * physical memory at allocation time (a user page fault goes straight to the
 * fatal path, there is no demand-paging handler), it never reclaims, pages out,
 * or swaps a process's memory, and it has no OOM killer. So under pressure a
 * new allocation returns a clean ENOMEM, and already-allocated memory is never
 * taken away. This probe validates that policy without needing a second
 * process: a background hog thread maps and writes anonymous memory until mmap
 * fails with MAP_FAILED, while a SCHED_FIFO realtime thread runs a periodic
 * loop that measures its wake latency and re-verifies a canary buffer it filled
 * before the pressure began. The hog is created before the main thread raises
 * itself to SCHED_FIFO, so the hog runs at normal priority and the realtime
 * thread preempts it (a hog spawned after the raise would inherit realtime
 * priority).
 *
 * Pass criteria: the hog reaches ENOMEM without a crash (admission control is
 * bounded and clean), the canary stays intact throughout and at peak pressure
 * (nothing reclaimed the realtime thread's memory), and the realtime loop keeps
 * running (pressure does not starve it). The realtime wake deviation is
 * reported but not gated, because absolute latency under QEMU/TCG is inflated.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>

#define CANARY_BYTES (64u * 1024u)
#define HOG_CHUNK (256u * 1024u)
#define HOG_MAX_CHUNKS 8192u  /* 2 GiB ceiling; ENOMEM hits far sooner */
#define RT_PERIOD_NS 1000000L /* 1 ms */
#define RT_MIN_ITERS 100      /* always measure at least this many periods */
#define RT_MAX_ITERS 4000     /* 4 s ceiling if the hog is slow */
#define RT_PRIORITY 60

/* Written by the hog thread, read by the main thread while both run, so every
 * access goes through __atomic to avoid a data race. hog_done carries the
 * release/acquire edge; the payload fields are relaxed (only read meaningfully
 * once done or after join).
 */
static int hog_done;
static int hog_enomem;
static unsigned long hog_bytes;
static char *hog_chunks[HOG_MAX_CHUNKS];
static unsigned char canary[CANARY_BYTES];

static unsigned char canary_byte(unsigned i)
{
    return (unsigned char) (i * 31u + 7u);
}

static int canary_intact(const unsigned char *c)
{
    for (unsigned i = 0; i < CANARY_BYTES; i++) {
        if (c[i] != canary_byte(i))
            return 0;
    }
    return 1;
}

static int monotonic_ns(uint64_t *out)
{
    if (__clock_time(CLOCK_MONOTONIC, NULL, out) == -1) {
        fprintf(stderr, "clock_time: %s\n", strerror(errno));
        return -1;
    }
    return 0;
}

static void ns_to_ts(uint64_t ns, struct timespec *ts)
{
    ts->tv_sec = (time_t) (ns / 1000000000ULL);
    ts->tv_nsec = (long) (ns % 1000000000ULL);
}

/* Map and write anonymous memory until the map fails, then stop and hold it so
 * the pressure stays applied while the realtime thread runs. Anonymous mmap
 * consumes real physical RAM (malloc only grows a small per-process arena).
 * Under the eager admission-control policy mmap returns MAP_FAILED cleanly at
 * exhaustion; the write confirms the memory is real and never faults (it is
 * already backed, since there is no demand-paging handler).
 */
static void *hog_thread(void *arg)
{
    unsigned n = 0;

    (void) arg;
    while (n < HOG_MAX_CHUNKS) {
        char *p = mmap(NULL, HOG_CHUNK, PROT_READ | PROT_WRITE,
                       MAP_ANON | MAP_PRIVATE, -1, 0);
        if (p == MAP_FAILED) {
            __atomic_store_n(&hog_enomem, 1, __ATOMIC_RELAXED);
            break;
        }
        memset(p, 0x5a, HOG_CHUNK);
        hog_chunks[n++] = p;
        __atomic_store_n(&hog_bytes, (unsigned long) n * HOG_CHUNK,
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&hog_done, 1, __ATOMIC_RELEASE);
    return NULL;
}

int main(void)
{
    pthread_t hog;
    struct sched_param sp = {.sched_priority = RT_PRIORITY};
    uint64_t start_ns, deadline_ns, max_dev = 0;
    int iters = 0, iters_under_pressure = 0, canary_ok = 1, err;

    /* Fill and touch the canary up front, so it is fully backed before the hog
     * applies pressure. The eager policy then never reclaims it.
     */
    for (unsigned i = 0; i < CANARY_BYTES; i++)
        canary[i] = canary_byte(i);

    /* Start the hog first, at the caller's normal priority, so it does not
     * inherit the SCHED_FIFO priority set below and the realtime thread truly
     * preempts it under pressure.
     */
    err = pthread_create(&hog, NULL, hog_thread, NULL);
    if (err != EOK) {
        fprintf(stderr, "mem-pressure: pthread_create: %s\n", strerror(err));
        return 1;
    }

    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1) {
        fprintf(stderr, "mem-pressure: __sched_set: %s\n", strerror(errno));
        return 1;
    }

    if (monotonic_ns(&start_ns) != 0)
        return 1;
    deadline_ns = start_ns;

    /* Periodic realtime loop during the hog's ramp-up. Each iteration sleeps to
     * an absolute deadline (so overhead does not compound), measures the wake
     * deviation, and re-verifies the canary. Ends when the hog reaches ENOMEM
     * or the iteration ceiling, whichever comes first.
     */
    while (iters < RT_MIN_ITERS ||
           (!__atomic_load_n(&hog_done, __ATOMIC_ACQUIRE) &&
            iters < RT_MAX_ITERS)) {
        uint64_t now_ns, dev;
        struct timespec wait;

        /* Count periods that began while the hog was still applying pressure,
         * so the starvation check below asserts real overlap rather than the
         * always-true iters > 0.
         */
        if (!__atomic_load_n(&hog_done, __ATOMIC_ACQUIRE))
            iters_under_pressure++;

        deadline_ns += (uint64_t) RT_PERIOD_NS;
        if (monotonic_ns(&now_ns) != 0)
            return 1;
        if (deadline_ns > now_ns) {
            ns_to_ts(deadline_ns - now_ns, &wait);
            if (nanosleep(&wait, NULL) != 0 && errno != EINTR) {
                fprintf(stderr, "mem-pressure: nanosleep: %s\n",
                        strerror(errno));
                return 1;
            }
        }
        if (monotonic_ns(&now_ns) != 0)
            return 1;
        dev = now_ns > deadline_ns ? now_ns - deadline_ns : deadline_ns - now_ns;
        if (dev > max_dev)
            max_dev = dev;
        if (!canary_intact(canary))
            canary_ok = 0;
        iters++;
    }

    /* Wait for the hog to finish exhausting memory (peak pressure), then verify
     * the canary one last time at the peak before the hog's memory is freed.
     */
    (void) pthread_join(hog, NULL);
    if (!canary_intact(canary))
        canary_ok = 0;

    for (unsigned i = 0; i < HOG_MAX_CHUNKS && hog_chunks[i]; i++)
        (void) munmap(hog_chunks[i], HOG_CHUNK);

    printf("mem-pressure: hog_bytes=%lu hog_enomem=%d rt_iters=%d "
           "rt_iters_under_pressure=%d rt_max_dev_ns=%llu canary_ok=%d\n",
           __atomic_load_n(&hog_bytes, __ATOMIC_RELAXED),
           __atomic_load_n(&hog_enomem, __ATOMIC_RELAXED), iters,
           iters_under_pressure, (unsigned long long) max_dev, canary_ok);

    if (!__atomic_load_n(&hog_enomem, __ATOMIC_RELAXED)) {
        fprintf(stderr, "mem-pressure: FAIL hog never hit ENOMEM (unbounded "
                        "admission or ceiling too low)\n");
        return 1;
    }
    if (!canary_ok) {
        fprintf(stderr, "mem-pressure: FAIL canary corrupted (memory was "
                        "reclaimed under pressure)\n");
        return 1;
    }
    if (iters_under_pressure == 0) {
        fprintf(stderr, "mem-pressure: FAIL realtime loop never ran while the "
                        "hog was applying pressure\n");
        return 1;
    }
    return 0;
}
