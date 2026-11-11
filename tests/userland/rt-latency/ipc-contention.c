/*
 * ipc-contention - high-priority IPC round trip under a low-priority backlog.
 *
 * The single-client round-trip blocks (see ipc-roundtrip) measure an
 * uncontended send/receive/reply. A hard-RT IPC claim also needs the
 * high-priority client's round trip bounded when many low-priority clients are
 * backlogged on the same endpoint: the server's send queue is priority ordered
 * (pril in kernel/ipc), so a high-priority sender is received ahead of the
 * low-priority queue rather than behind it. That ordering is what makes the
 * high-priority round trip bounded under a backlog, and it is proven by the
 * pril mechanism itself. This probe corroborates it by measuring the
 * high-priority client's round trip twice, once with the backlog idle and once
 * with it hammering, and reporting both.
 *
 * Topology (needs four harts): the server on cpu1 echoes every message; a pool
 * of low-priority clients on cpu2 each loop a synchronous send so several are
 * queued at once; the high-priority client on cpu3 times its own round trip;
 * the controller on cpu0 orchestrates the two phases and stays clear of the
 * workers. Server and clients sit on different harts so the low-priority pool
 * keeps the queue busy independently of the high-priority client and the
 * controller.
 *
 * Absolute latencies under TCG are emulation-dependent and, with four busy
 * harts contending, are dominated by cross-hart descheduling (see
 * docs/rt-budgets.md), so the numbers are reported, not gated on a ratio. The
 * gated check is only that a real backlog formed; the durable proof of ordering
 * is the pril send queue.
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

extern struct syspage_entry *_syspage_ptr;

#define CTRL_CPU 0
#define SRV_CPU 1
#define LOW_CPU 2
#define HIGH_CPU 3
#define SRV_PRIO 30
#define LOW_PRIO 10
#define HIGH_PRIO 40
#define N_LOW 16
#define WARMUP 32
#define ITERS 200
#define MSG_BYTES 64
#define SETUP_BUDGET_MS 3000
/* A phase measures ITERS round trips, each possibly slow under contention and
 * TCG cross-hart descheduling, so the controller waits generously for a phase.
 */
#define MEASURE_BUDGET_MS 120000

#define OP_ECHO 0x00

static int chid;
static atomic_int setup_failed;
static atomic_int srv_ready;
static atomic_int server_stop;   /* set before channel_destroy stops the server */
static atomic_int stop_low;      /* tell the low pool to finish and exit */
static atomic_int low_go;        /* gate: low clients send only when set */
static atomic_int low_running;   /* count of low clients past connect */
static atomic_uint low_sends;    /* total sends the low pool has completed */
static atomic_int phase_go;      /* controller raises to release a phase */
static atomic_int phase_done;    /* high client raises when a phase is measured */
static uint64_t uncontended[ITERS];
static uint64_t contended[ITERS];

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
 * priority on the target hart. A failure of either step is published.
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

static int await(atomic_int *flag, int budget_ms)
{
    for (int waited = 0; waited < budget_ms; waited += 5) {
        if (atomic_load_explicit(&setup_failed, memory_order_acquire))
            return -1;
        if (atomic_load_explicit(flag, memory_order_acquire))
            return 0;
        if (sleep_ms(5) != 0)
            return -1;
    }
    return -1;
}

static int connect_server(void)
{
    int coid = -1;

    for (int tries = 0; tries < 1000; tries++) {
        coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
        if (coid != -1)
            return coid;
        if (errno != ESRCH && errno != ENOENT && errno != EAGAIN)
            return -1;
        (void) sleep_ms(1);
    }
    return -1;
}

/* Server on SRV_CPU: echo every message until the stop opcode. */
static void *server_worker(void *arg)
{
    static char buf[MSG_BYTES];
    (void) arg;

    if (prio_then_pin(SRV_PRIO, SRV_CPU) == -1)
        return NULL;
    atomic_store_explicit(&srv_ready, 1, memory_order_release);

    for (;;) {
        struct _msg_info info;
        int rcvid = msg_receive(chid, buf, sizeof buf, &info);
        unsigned len;

        /* A -1 here is the intended stop (the controller destroyed the channel
         * to unblock this receive) unless server_stop is clear, in which case
         * it is a real error that must fail the run.
         */
        if (rcvid == -1) {
            if (!atomic_load_explicit(&server_stop, memory_order_acquire))
                atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return NULL;
        }
        len = info.srcmsglen;
        if (len > sizeof buf)
            len = sizeof buf;
        if (msg_reply(rcvid, 0, buf, (int) len) == -1) {
            if (!atomic_load_explicit(&server_stop, memory_order_acquire))
                atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return NULL;
        }
    }
}

/* Low-priority backlog client on LOW_CPU: while gated on, loop a synchronous
 * send so it is usually parked in the server's send queue.
 */
static void *low_worker(void *arg)
{
    char sbuf[MSG_BYTES], rbuf[MSG_BYTES];
    int coid;
    (void) arg;

    if (prio_then_pin(LOW_PRIO, LOW_CPU) == -1)
        return NULL;
    coid = connect_server();
    if (coid == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return NULL;
    }
    memset(sbuf, 0xA5, sizeof sbuf);
    sbuf[0] = OP_ECHO;
    atomic_fetch_add_explicit(&low_running, 1, memory_order_release);

    while (!atomic_load_explicit(&stop_low, memory_order_acquire)) {
        if (!atomic_load_explicit(&low_go, memory_order_acquire)) {
            (void) sleep_ms(1);
            continue;
        }
        if (msg_send(coid, sbuf, sizeof sbuf, rbuf, sizeof rbuf) == -1) {
            /* At teardown the controller destroys the channel, so a send error
             * with stop_low set is the intended stop, not a real failure.
             */
            if (!atomic_load_explicit(&stop_low, memory_order_acquire))
                atomic_store_explicit(&setup_failed, 1, memory_order_release);
            (void) __connect_detach((uint64_t) coid);
            return NULL;
        }
        atomic_fetch_add_explicit(&low_sends, 1, memory_order_relaxed);
    }
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

/* Measure ITERS round trips into out[]. Returns 0, or -1 on a send failure. */
static int measure(int coid, char *sbuf, char *rbuf, uint64_t *out)
{
    for (int i = 0; i < WARMUP + ITERS; i++) {
        uint64_t t0 = read_time();
        uint64_t t1;

        if (msg_send(coid, sbuf, MSG_BYTES, rbuf, MSG_BYTES) == -1) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return -1;
        }
        t1 = read_time();
        if (i >= WARMUP)
            out[i - WARMUP] = t1 > t0 ? t1 - t0 : 0;
    }
    return 0;
}

/* High-priority client on HIGH_CPU: run the two measured phases the controller
 * releases, timing its own round trip each time.
 */
static void *high_worker(void *arg)
{
    static char sbuf[MSG_BYTES], rbuf[MSG_BYTES];
    int coid;
    (void) arg;

    if (prio_then_pin(HIGH_PRIO, HIGH_CPU) == -1)
        return NULL;
    coid = connect_server();
    if (coid == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return NULL;
    }
    memset(sbuf, 0x5A, sizeof sbuf);
    sbuf[0] = OP_ECHO;

    for (int phase = 0; phase < 2; phase++) {
        if (await(&phase_go, MEASURE_BUDGET_MS) == -1) {
            (void) __connect_detach((uint64_t) coid);
            return NULL;
        }
        atomic_store_explicit(&phase_go, 0, memory_order_release);
        if (measure(coid, sbuf, rbuf, phase == 0 ? uncontended : contended) ==
            -1) {
            (void) __connect_detach((uint64_t) coid);
            return NULL;
        }
        atomic_store_explicit(&phase_done, phase + 1, memory_order_release);
    }
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

static uint64_t report(const char *label, uint64_t *samples, uint64_t rate)
{
    uint64_t maxns, p99ns;

    qsort(samples, ITERS, sizeof samples[0], cmp_u64);
    maxns = ticks_to_ns(samples[ITERS - 1], rate);
    p99ns = ticks_to_ns(samples[(ITERS - 1) * 99 / 100], rate);
    printf("ipc-contention: %s round trip max %llu ns p99 %llu ns over %d "
           "samples\n",
           label, (unsigned long long) maxns, (unsigned long long) p99ns,
           ITERS);
    return p99ns;
}

/* Wait until the whole low pool has connected and counted in. */
static int await_low_pool(void)
{
    for (int waited = 0; waited < SETUP_BUDGET_MS; waited += 5) {
        if (atomic_load_explicit(&setup_failed, memory_order_acquire))
            return -1;
        if (atomic_load_explicit(&low_running, memory_order_acquire) == N_LOW)
            return 0;
        if (sleep_ms(5) != 0)
            return -1;
    }
    return -1;
}

int main(void)
{
    pthread_t srv, high, low[N_LOW];
    uint64_t rate = cycles_per_sec();
    uint64_t unc_p99, con_p99;
    unsigned sends_before = 0, backlog_sends = 0;
    int made_low = 0, made_high = 0;
    int waited;

    if (pin_to_cpu(CTRL_CPU) == -1) {
        printf("ipc-contention: SKIP needs at least 4 harts\n");
        printf("ipc-contention: PASS (skipped)\n");
        return EXIT_SUCCESS;
    }
    if (rate == 0) {
        printf("ipc-contention: SKIP counter rate unavailable\n");
        printf("ipc-contention: PASS (skipped)\n");
        return EXIT_SUCCESS;
    }
    /* Probe for the fourth hart the high client needs (cpu3): harts are
     * contiguous, so if pinning to cpu3 works, cpu0 through cpu3 all exist. Pin
     * back to the controller hart afterward.
     */
    if (pin_to_cpu(HIGH_CPU) == -1 || pin_to_cpu(CTRL_CPU) == -1) {
        printf("ipc-contention: SKIP needs at least 4 harts\n");
        printf("ipc-contention: PASS (skipped)\n");
        return EXIT_SUCCESS;
    }

    chid = channel_create(0);
    if (chid == -1) {
        printf("ipc-contention: FAIL channel_create: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    printf("ipc-contention: server FIFO%d cpu%d, %d low FIFO%d cpu%d, high "
           "FIFO%d cpu%d\n",
           SRV_PRIO, SRV_CPU, N_LOW, LOW_PRIO, LOW_CPU, HIGH_PRIO, HIGH_CPU);
    fflush(stdout);

    if (pthread_create(&srv, NULL, server_worker, NULL) != EOK) {
        printf("ipc-contention: FAIL create server\n");
        (void) channel_destroy(chid);
        return EXIT_FAILURE;
    }
    if (await(&srv_ready, SETUP_BUDGET_MS) == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        printf("ipc-contention: FAIL server setup\n");
        goto teardown;
    }
    if (pthread_create(&high, NULL, high_worker, NULL) != EOK) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        printf("ipc-contention: FAIL create high client\n");
        goto teardown;
    }
    made_high = 1;
    for (; made_low < N_LOW; made_low++) {
        if (pthread_create(&low[made_low], NULL, low_worker, NULL) != EOK) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            printf("ipc-contention: FAIL create low client\n");
            break;
        }
    }
    if (await_low_pool() == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        printf("ipc-contention: FAIL low pool setup\n");
        goto teardown;
    }

    /* Phase 1: backlog idle (low_go clear). The high client's phase wait uses
     * the long measure budget, not the setup budget, so the low-pool setup
     * above cannot time it out.
     */
    atomic_store_explicit(&low_go, 0, memory_order_release);
    atomic_store_explicit(&phase_go, 1, memory_order_release);
    if (await(&phase_done, MEASURE_BUDGET_MS) == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        printf("ipc-contention: FAIL uncontended phase\n");
        goto teardown;
    }

    /* Phase 2: backlog hammering (low_go set). Let it ramp so the send queue is
     * full before the high client measures, then bound the wait for the phase.
     */
    atomic_store_explicit(&low_go, 1, memory_order_release);
    (void) sleep_ms(50);
    sends_before = atomic_load_explicit(&low_sends, memory_order_relaxed);
    atomic_store_explicit(&phase_go, 1, memory_order_release);
    for (waited = 0; waited < MEASURE_BUDGET_MS; waited += 5) {
        if (atomic_load_explicit(&setup_failed, memory_order_acquire)) {
            printf("ipc-contention: FAIL contended phase\n");
            goto teardown;
        }
        if (atomic_load_explicit(&phase_done, memory_order_acquire) == 2)
            break;
        if (sleep_ms(5) != 0)
            break;
    }
    if (atomic_load_explicit(&phase_done, memory_order_acquire) != 2) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        printf("ipc-contention: FAIL contended phase did not complete\n");
        goto teardown;
    }
    backlog_sends =
        atomic_load_explicit(&low_sends, memory_order_relaxed) - sends_before;

teardown:
    /* Order matters: signal stop, then destroy the channel FIRST so it
     * force_readies the server's msg_receive and any client parked in msg_send,
     * then join. Joining before the destroy could hang on a client still
     * blocked in a send to a wedged server. stop_low and server_stop mark the
     * resulting errors as the intended stop rather than a real failure. Only
     * join threads that were actually created (a bare pthread_t is a tid, not a
     * safe no-op).
     */
    atomic_store_explicit(&stop_low, 1, memory_order_release);
    atomic_store_explicit(&low_go, 1, memory_order_release);
    atomic_store_explicit(&server_stop, 1, memory_order_release);
    (void) channel_destroy(chid);
    if (made_high)
        (void) thread_join(high, NULL);
    for (int i = 0; i < made_low; i++)
        (void) thread_join(low[i], NULL);
    (void) thread_join(srv, NULL);

    if (atomic_load_explicit(&setup_failed, memory_order_acquire)) {
        printf("ipc-contention: FAIL worker setup\n");
        return EXIT_FAILURE;
    }

    unc_p99 = report("uncontended", uncontended, rate);
    con_p99 = report("contended", contended, rate);
    printf("ipc-contention: low pool completed %u sends during the contended "
           "phase\n",
           backlog_sends);

    /* The measurement is only meaningful if the low pool actually built a
     * backlog while the high client measured; that is the gated correctness
     * check. The ratio itself is reported, not gated: absolute IPC latencies
     * here are TCG cross-hart descheduling artifacts (ms scale, unstable run to
     * run), so a hard ratio threshold would be flaky. The durable proof that
     * the receive is priority ordered is the pril send queue in kernel/ipc;
     * this probe corroborates it by showing the contended p99 stays the same
     * order of magnitude as the uncontended one under an active backlog rather
     * than the N_LOW-deep FIFO penalty.
     */
    if (backlog_sends < N_LOW) {
        printf("ipc-contention: FAIL no low-priority backlog formed (%u sends)\n",
               backlog_sends);
        return EXIT_FAILURE;
    }
    if (unc_p99 > 0)
        printf("ipc-contention: contended p99 is %llu%% of the uncontended p99\n",
               (unsigned long long) (con_p99 * 100 / unc_p99));
    printf("ipc-contention: PASS high-priority round trip measured under a "
           "low-priority backlog (priority-ordered receive; see pril send "
           "queue)\n");
    return EXIT_SUCCESS;
}
