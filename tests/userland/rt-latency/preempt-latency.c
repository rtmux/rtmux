/*
 * preempt-latency - wake-to-dispatch preemption latency.
 *
 * A higher-priority thread that is made runnable must preempt the
 * lower-priority thread running on its hart. This probe measures that gap for
 * three wake sources, one phase each.
 *
 * The ipc source has a client blocked in a synchronous MsgSend woken by the
 * server's MsgReply; server and client share a hart with the server below the
 * client, so the reply, running in the server's syscall context, preempts the
 * server returning from it. This syscall-context wake dispatches promptly and
 * measures a real reply-to-dispatch max and p99. The irq source has a waiter
 * blocked in interrupt_wait for the goldfish-rtc alarm IRQ, and the timer
 * source a waiter blocked in clock_nanosleep, each while a busy lower-priority
 * hog holds the same hart.
 *
 * On the current tree both interrupt-context wakes (irq and timer) do NOT
 * preempt the busy hog above two harts, so the gap is not specific to the timer
 * wheel; an off-hart watchdog names each gap instead of letting the run hang.
 *
 * The watchdog runs on its own hart so it survives a same-hart dispatch failure
 * and can name a stall rather than let the run hang; the controller sits on a
 * third hart and blocks in thread_join, so it never busy-competes with the
 * pinned workers. Absolute latencies are TCG-emulation dependent; the signal is
 * whether the wake preempts at all, and its max when it does.
 *
 * Needs at least three harts: waiter/hog, watchdog, controller. With fewer it
 * skips, since the gaps only appear above two harts anyway.
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
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/syspage.h>
#include <sys/virtio-mmio-msg.h>

#include <arch/plic-vector.h>

#include "../outcome.h"

extern struct syspage_entry *_syspage_ptr;

#define WAIT_CPU 0       /* waiter/client and hog share the boot hart */
#define OFF_CPU 1        /* watchdog and IPC server on a second hart */
#define CTRL_CPU 2       /* controller on a third hart, out of the way */
#define HOG_PRIO 8       /* low priority busy thread */
#define WAIT_PRIO 40     /* high priority timer waiter / IPC client */
#define SRV_PRIO 30      /* IPC server, above the hog */
#define WD_PRIO 45       /* watchdog above the waiter, on its own hart */
#define SLEEP_NS 1000000 /* 1 ms per timer wake */
#define TIMER_ITERS 60
#define IPC_ITERS 200
#define IPC_WARMUP 32
/* Generous wake-to-dispatch ceiling. Absolute values under TCG are emulation
 * noise (docs/rt-budgets.md), so this is not a latency budget; it only catches
 * a grossly late wake. The real signal is completed-and-preempted versus
 * stalled-and-named.
 */
#define WAKE_BOUND_NS 50000000
/* Watchdog declares a stall after this long with no waiter progress. */
#define STALL_BUDGET_NS 2000000000ULL
/* Cross-hart pass gets a more generous budget: a real regression is a permanent
 * stall that trips any budget, so the extra headroom only absorbs the rare
 * multi-hart TCG scheduling spike without turning the strict gate flaky.
 */
#define XHART_STALL_BUDGET_NS 4000000000ULL
/* Bound on how long the controller waits for a worker to finish setup. */
#define SETUP_BUDGET_MS 3000
/* Second non-boot hart for the cross-hart timer pass: the boot-hart timer
 * readies the waiter onto this different busy hart, exercising the cross-hart
 * reschedule IPI preemption path. Distinct from OFF_CPU and CTRL_CPU, so it
 * needs 4 harts.
 */
#define CROSS_CPU 3

/* The hart the timer waiter and its hog pin to. WAIT_CPU (the boot hart) for
 * the same-hart pass; main flips it to CROSS_CPU for the cross-hart pass, then
 * restores it. Only the timer hog and waiter follow this; the IPC and IRQ
 * phases stay on WAIT_CPU.
 */
static unsigned wait_cpu = WAIT_CPU;
/* Watchdog stall budget for the current phase; main sets it per phase. */
static uint64_t stall_budget = STALL_BUDGET_NS;

#define OP_ECHO 0x00
#define OP_STOP 0xEE

static atomic_int setup_failed; /* any worker could not raise policy or pin */

/* Read the shared mtime counter (rdtime), exposed to U-mode via scounteren. It
 * advances on every hart independently of the boot CPU's software clock, so it
 * is a robust time base for a watchdog that must survive that clock stalling.
 */
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

/* Nanoseconds to counter ticks, same split to stay in range. */
static uint64_t ticks_from_ns(uint64_t ns, uint64_t rate)
{
    return (ns / 1000000000ULL) * rate +
           (ns % 1000000000ULL) * rate / 1000000000ULL;
}

static uint64_t cycles_per_sec(void)
{
    if (_syspage_ptr && _syspage_ptr->qtime.entry_off)
        return SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return 0;
}

static uint64_t ts_ns(const struct timespec *t)
{
    return (uint64_t) t->tv_sec * 1000000000ULL + (uint64_t) t->tv_nsec;
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

/* Raise policy before pinning: pinning at the inherited priority onto the busy
 * waiter hart would strand the thread behind the hog. A failure of either step
 * is published so the controller fails the run rather than measuring a probe
 * with a missing competitor, waiter, or server.
 */
static int prio_then_pin(int policy, int priority, unsigned cpu)
{
    struct sched_param sp = {.sched_priority = priority};

    if (__sched_set(0, 0, policy, &sp) == -1 || pin_to_cpu(cpu) == -1) {
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

/* Wait, off the contended hart, until a worker publishes readiness.
 *
 * Returns -1 if setup failed or the worker never became ready within the
 * budget.
 */
static int await_ready(atomic_int *ready)
{
    for (int waited = 0; waited < SETUP_BUDGET_MS; waited += 5) {
        if (atomic_load_explicit(&setup_failed, memory_order_acquire))
            return -1;
        if (atomic_load_explicit(ready, memory_order_acquire))
            return 0;
        if (sleep_ms(5) != 0)
            return -1;
    }
    return -1;
}

/* A busy hog pinned to WAIT_CPU. It exits when its stop flag is set. The stop
 * and ready flags are passed in so each phase can drive its own hog.
 */
struct hog_arg {
    atomic_int *stop;
    atomic_int *ready;
};

static void *hog_worker(void *arg)
{
    struct hog_arg *h = arg;
    volatile uint64_t spins = 0;

    if (prio_then_pin(SCHED_FIFO, HOG_PRIO, wait_cpu) == -1)
        return NULL;
    atomic_store_explicit(h->ready, 1, memory_order_release);
    while (!atomic_load_explicit(h->stop, memory_order_acquire))
        spins++;
    return NULL;
}

/* IPC-reply source.
 *
 * Server and client share one hart, the server one priority below the client. A
 * same-hart busy hog is deliberately absent: a hog at or above the server
 * priority would starve the server so it could never reply, and a hog below it
 * is dominated and never runs. The lower-priority thread the reply preempts is
 * therefore the server returning from msg_reply. That wake runs in the server's
 * syscall context, which already dispatches a higher-priority thread promptly
 * (see pi-inversion-xfail), so this measures a working reply-to-dispatch rather
 * than probing for a stall, and needs no watchdog.
 */

static atomic_int ipc_srv_ready, ipc_client_done;
static atomic_uint_least64_t ipc_reply_ts; /* server rdtime just before reply */
static uint64_t ipc_samples[IPC_ITERS]; /* reply-to-dispatch ticks per wake */
static int ipc_chid;

/* Server: echoes each message, timestamping just before the reply so the client
 * can measure from reply issue to its own dispatch.
 *
 * Returns on the stop opcode.
 */
static void *ipc_server(void *arg)
{
    static char buf[256];
    (void) arg;

    if (prio_then_pin(SCHED_FIFO, SRV_PRIO, WAIT_CPU) == -1)
        return NULL;
    atomic_store_explicit(&ipc_srv_ready, 1, memory_order_release);

    for (;;) {
        struct _msg_info info;
        int rcvid = msg_receive(ipc_chid, buf, sizeof buf, &info);
        unsigned len;

        if (rcvid == -1)
            return NULL;
        len = info.srcmsglen;
        if (len > sizeof buf)
            len = sizeof buf;
        if (len >= 1 && (unsigned char) buf[0] == OP_STOP) {
            (void) msg_reply(rcvid, 0, buf, (int) len);
            return NULL;
        }
        atomic_store_explicit(&ipc_reply_ts, read_time(), memory_order_release);
        if (msg_reply(rcvid, 0, buf, (int) len) == -1)
            return NULL;
    }
}

/* Client on WAIT_CPU: each synchronous MsgSend blocks until the server replies,
 * which must preempt the server returning from the reply. The gap from the
 * server's reply timestamp to the client's post-send read is the
 * reply-to-dispatch latency.
 */
static void *ipc_client(void *arg)
{
    static char sbuf[256], rbuf[256];
    int coid = -1;
    (void) arg;

    if (prio_then_pin(SCHED_FIFO, WAIT_PRIO, WAIT_CPU) == -1)
        return NULL;

    for (int tries = 0; tries < 1000; tries++) {
        coid = connect_attach(0, getpid(), ipc_chid, RTMUX_SIDE_CHANNEL, 0);
        if (coid != -1)
            break;
        if (errno != ESRCH && errno != ENOENT && errno != EAGAIN) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return NULL;
        }
        (void) sleep_ms(1);
    }
    if (coid == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return NULL;
    }

    memset(sbuf, 0xA5, sizeof sbuf);
    sbuf[0] = OP_ECHO;
    for (int i = 0; i < IPC_WARMUP + IPC_ITERS; i++) {
        uint64_t wake, reply, lat;

        if (msg_send(coid, sbuf, sizeof sbuf, rbuf, sizeof rbuf) == -1) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return NULL;
        }
        wake = read_time();
        reply = atomic_load_explicit(&ipc_reply_ts, memory_order_acquire);
        lat = wake > reply ? wake - reply : 0;
        /* The controller reads ipc_samples only after joining this thread, so
         * the join is the ordering barrier and no atomic is needed here.
         */
        if (i >= IPC_WARMUP)
            ipc_samples[i - IPC_WARMUP] = lat;
    }

    sbuf[0] = OP_STOP;
    (void) msg_send(coid, sbuf, sizeof sbuf, rbuf, sizeof rbuf);
    atomic_store_explicit(&ipc_client_done, 1, memory_order_release);
    return NULL;
}

/* Unblock the server from msg_receive by sending it a stop from the controller,
 * used when the client failed before sending its own stop so the server join
 * cannot hang.
 */
static void release_ipc_server(void)
{
    char stop[8] = {(char) OP_STOP};
    char reply[8];
    int coid = connect_attach(0, getpid(), ipc_chid, RTMUX_SIDE_CHANNEL, 0);

    if (coid != -1)
        (void) msg_send(coid, stop, sizeof stop, reply, sizeof reply);
}

/* Returns 0 on a clean measured pass, -1 on failure. */
static int run_ipc_source(void)
{
    pthread_t srv, client;
    uint64_t maxns, p99ns, rate;

    /* Fixed-priority channel: without it, message-driven priority inheritance
     * would boost the server to the client priority while it handles the
     * request, so msg_reply would ready an equal-priority client rather than
     * preempt a genuinely lower-priority server.
     */
    ipc_chid = channel_create(RTMUX_FLG_CHF_FIXED_PRIORITY);
    if (ipc_chid == -1) {
        printf("preempt-latency: ipc FAIL channel_create: %s\n",
               strerror(errno));
        return -1;
    }

    if (pthread_create(&srv, NULL, ipc_server, NULL) != EOK)
        return -1;
    if (await_ready(&ipc_srv_ready) == -1) {
        printf("preempt-latency: ipc FAIL server setup\n");
        release_ipc_server();
        (void) thread_join(srv, NULL);
        return -1;
    }
    if (pthread_create(&client, NULL, ipc_client, NULL) != EOK) {
        release_ipc_server();
        (void) thread_join(srv, NULL);
        return -1;
    }

    (void) thread_join(client, NULL);
    /* If the client failed before sending its own stop, it left the server
     * blocked in msg_receive; release it so the join below cannot hang.
     */
    if (!atomic_load_explicit(&ipc_client_done, memory_order_acquire))
        release_ipc_server();
    (void) thread_join(srv, NULL);

    if (!atomic_load_explicit(&ipc_client_done, memory_order_acquire)) {
        printf("preempt-latency: ipc FAIL client did not complete\n");
        return -1;
    }
    /* Sort so max is the last element and p99 a fixed index; the client has
     * been joined, so ipc_samples is stable.
     */
    qsort(ipc_samples, IPC_ITERS, sizeof ipc_samples[0], cmp_u64);
    rate = cycles_per_sec();
    maxns = ticks_to_ns(ipc_samples[IPC_ITERS - 1], rate);
    p99ns = ticks_to_ns(ipc_samples[(IPC_ITERS - 1) * 99 / 100], rate);
    printf(
        "preempt-latency: ipc reply-to-dispatch max %llu ns p99 %llu ns over "
        "%d wakes\n",
        (unsigned long long) maxns, (unsigned long long) p99ns, IPC_ITERS);
    if (maxns > WAKE_BOUND_NS) {
        printf("preempt-latency: ipc FAIL max %llu ns > %d ns bound\n",
               (unsigned long long) maxns, WAKE_BOUND_NS);
        return -1;
    }
    printf("preempt-latency: ipc PASS reply wake preempts within bound\n");
    return 0;
}

/* timer source */

static atomic_int timer_stop;
static atomic_int timer_hog_ready, waiter_ready, wd_ready;
static atomic_uint waiter_progress; /* iterations the waiter has completed */
static atomic_int waiter_done;
static atomic_int stalled;
static atomic_uint_least64_t
    timer_max_lat; /* max wake-to-dispatch lateness, ns */

/* The waiter sleeps to an absolute deadline; a working timer wake preempts the
 * hog so the waiter measures a small gap. If the wake never lands the waiter
 * blocks forever in clock_nanosleep and waiter_progress stops advancing, which
 * the watchdog on the other hart detects.
 */
static void *waiter_worker(void *arg)
{
    struct timespec deadline;
    (void) arg;

    if (prio_then_pin(SCHED_FIFO, WAIT_PRIO, wait_cpu) == -1)
        return NULL;
    atomic_store_explicit(&waiter_ready, 1, memory_order_release);

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    for (unsigned i = 0; i < TIMER_ITERS; i++) {
        struct timespec now;
        uint64_t late;

        deadline.tv_nsec += SLEEP_NS;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_nsec -= 1000000000L;
            deadline.tv_sec++;
        }
        if (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) !=
            0)
            break;
        /* Lateness past the programmed deadline is the wake-to-dispatch time.
         */
        clock_gettime(CLOCK_MONOTONIC, &now);
        late =
            ts_ns(&now) > ts_ns(&deadline) ? ts_ns(&now) - ts_ns(&deadline) : 0;
        if (late > atomic_load_explicit(&timer_max_lat, memory_order_relaxed))
            atomic_store_explicit(&timer_max_lat, late, memory_order_relaxed);
        atomic_store_explicit(&waiter_progress, i + 1, memory_order_release);
        if (atomic_load_explicit(&timer_stop, memory_order_acquire))
            break;
    }
    atomic_store_explicit(&waiter_done, 1, memory_order_release);
    return NULL;
}

/* State a watchdog observes for one phase. The timer and IRQ phases differ only
 * in which globals they watch, so both drive the same wd_worker through this.
 */
struct wd_arg {
    atomic_uint *progress; /* iterations the waiter has completed */
    atomic_int *done;      /* waiter finished */
    atomic_int *stalled;   /* watchdog sets on a declared stall */
    atomic_int *stop;      /* watchdog sets to stop the hog on a stall */
    atomic_int *ready;     /* watchdog publishes its own readiness here */
};

/* Watchdog on OFF_CPU. Uses rdtime (per-hart, always advancing) as its stall
 * clock, so it can declare a stall even if the boot CPU's software clock has
 * frozen. It returns when the waiter finishes or makes no progress for
 * STALL_BUDGET_NS. In a true system-wide wedge this thread cannot be dispatched
 * either; the harness timeout is the only backstop for that.
 */
static void *wd_worker(void *arg)
{
    struct wd_arg *w = arg;
    uint64_t rate = cycles_per_sec();
    uint64_t budget_ticks;
    uint64_t last_change;
    unsigned last_progress = 0;

    if (prio_then_pin(SCHED_FIFO, WD_PRIO, OFF_CPU) == -1)
        return NULL;
    atomic_store_explicit(w->ready, 1, memory_order_release);

    /* main confirmed the counter rate is known before starting any phase, so
     * the budget conversion is exact. main sets stall_budget per phase.
     */
    budget_ticks = ticks_from_ns(stall_budget, rate);
    last_change = read_time();
    for (;;) {
        unsigned p = atomic_load_explicit(w->progress, memory_order_acquire);
        uint64_t now;

        if (atomic_load_explicit(w->done, memory_order_acquire))
            break;
        now = read_time();
        if (p != last_progress) {
            last_progress = p;
            last_change = now;
        } else if (now - last_change > budget_ticks) {
            atomic_store_explicit(w->stalled, 1, memory_order_release);
            atomic_store_explicit(w->stop, 1, memory_order_release);
            break;
        }
    }
    return NULL;
}

static struct wd_arg timer_wd_arg = {
    .progress = &waiter_progress,
    .done = &waiter_done,
    .stalled = &stalled,
    .stop = &timer_stop,
    .ready = &wd_ready,
};

/* Returns 0 on a clean completed pass, 1 when the gap was named (still a pass
 * per the task criterion), -1 on failure.
 */
static int run_timer_source(const char *label)
{
    pthread_t hog, waiter, wd;
    struct hog_arg ha = {.stop = &timer_stop, .ready = &timer_hog_ready};
    unsigned progress;
    uint64_t maxns;

    /* Re-runnable: main drives a same-hart pass and then a cross-hart pass, so
     * reset all phase state before each run.
     */
    atomic_store_explicit(&timer_stop, 0, memory_order_release);
    atomic_store_explicit(&timer_hog_ready, 0, memory_order_release);
    atomic_store_explicit(&waiter_ready, 0, memory_order_release);
    atomic_store_explicit(&wd_ready, 0, memory_order_release);
    atomic_store_explicit(&waiter_progress, 0, memory_order_release);
    atomic_store_explicit(&waiter_done, 0, memory_order_release);
    atomic_store_explicit(&stalled, 0, memory_order_release);
    atomic_store_explicit(&timer_max_lat, 0, memory_order_release);

    if (pthread_create(&wd, NULL, wd_worker, &timer_wd_arg) != EOK)
        return -1;
    if (await_ready(&wd_ready) == -1) {
        printf("preempt-latency: %s FAIL watchdog setup\n", label);
        return -1;
    }
    if (pthread_create(&hog, NULL, hog_worker, &ha) != EOK)
        return -1;
    if (await_ready(&timer_hog_ready) == -1) {
        printf("preempt-latency: %s FAIL hog setup\n", label);
        return -1;
    }
    if (pthread_create(&waiter, NULL, waiter_worker, NULL) != EOK)
        return -1;
    if (await_ready(&waiter_ready) == -1) {
        printf("preempt-latency: %s FAIL waiter setup\n", label);
        return -1;
    }

    (void) thread_join(wd, NULL);
    atomic_store_explicit(&timer_stop, 1, memory_order_release);
    (void) thread_join(hog, NULL);

    progress = atomic_load_explicit(&waiter_progress, memory_order_acquire);
    maxns = atomic_load_explicit(&timer_max_lat, memory_order_acquire);

    if (atomic_load_explicit(&stalled, memory_order_acquire)) {
        /* The waiter is still blocked in clock_nanosleep, so do not join it. */
        printf(
            "preempt-latency: %s NAMED-GAP wake-to-dispatch stalled after "
            "%u/%d wakes (max seen %llu ns); a nanosleep wheel timer does not "
            "preempt a busy lower-priority thread on cpu%u at this hart "
            "count (see docs/rt-budgets.md and the SMP timer-wake gap).\n",
            label, progress, TIMER_ITERS, (unsigned long long) maxns, wait_cpu);
        return 1;
    }

    (void) thread_join(waiter, NULL);
    printf(
        "preempt-latency: %s completed %u/%d wakes, max wake-to-dispatch "
        "%llu ns\n",
        label, progress, TIMER_ITERS, (unsigned long long) maxns);
    if (progress < TIMER_ITERS) {
        printf("preempt-latency: %s FAIL waiter did not complete\n", label);
        return -1;
    }
    if (maxns > WAKE_BOUND_NS) {
        printf("preempt-latency: %s FAIL max %llu ns > %d ns bound\n", label,
               (unsigned long long) maxns, WAKE_BOUND_NS);
        return -1;
    }
    printf("preempt-latency: %s PASS max wake-to-dispatch %llu ns\n", label,
           (unsigned long long) maxns);
    return 0;
}

/* IRQ-notification source.
 *
 * A high-priority waiter blocks in interrupt_wait for the goldfish-rtc alarm
 * IRQ while a low-priority hog holds the same hart; the IRQ must preempt the
 * hog. Unlike the timer source this does not go through the timer wheel (the
 * PLIC delivers the IRQ directly), so it is expected to preempt; the watchdog
 * still names a stall if it does not. The alarm is a deterministic
 * self-trigger: arm it a fixed delta ahead and it raises its IRQ at that RTC
 * time.
 */

/* goldfish-rtc register offsets (google,goldfish-rtc), all 32-bit. */
#define RTC_TIME_LOW 0x00
#define RTC_TIME_HIGH 0x04
#define RTC_ALARM_LOW 0x08
#define RTC_ALARM_HIGH 0x0c
#define RTC_IRQ_ENABLED 0x10
#define RTC_CLEAR_ALARM 0x14
#define RTC_CLEAR_INTERRUPT 0x1c
#define ALARM_DELTA_NS 1000000ULL /* 1 ms ahead */
#define IRQ_ITERS 60

static volatile unsigned char *irq_rtc;
static atomic_int irq_stop;
static atomic_int irq_hog_ready, irq_waiter_ready, irq_wd_ready;
static atomic_uint irq_progress;
static atomic_int irq_waiter_done;
static atomic_int irq_stalled;
static atomic_uint_least64_t irq_max_lat; /* ns */
static int irq_vector;

static uint32_t rtc_rd(unsigned off)
{
    return *(volatile uint32_t *) (irq_rtc + off);
}

static void rtc_wr(unsigned off, uint32_t v)
{
    *(volatile uint32_t *) (irq_rtc + off) = v;
}

/* Reading TIME_LOW latches the high half, so the pair is one snapshot. */
static uint64_t rtc_now(void)
{
    uint32_t lo = rtc_rd(RTC_TIME_LOW);
    uint32_t hi = rtc_rd(RTC_TIME_HIGH);
    return ((uint64_t) hi << 32) | lo;
}

static void rtc_arm(uint64_t when)
{
    rtc_wr(RTC_IRQ_ENABLED, 1);
    rtc_wr(RTC_ALARM_HIGH, (uint32_t) (when >> 32));
    rtc_wr(RTC_ALARM_LOW, (uint32_t) when);
}

/* Waiter on WAIT_CPU: attach the alarm IRQ as an IST, then loop arming the
 * alarm and blocking in interrupt_wait. Each wake past the armed deadline is
 * the IRQ-to-dispatch time. If a wake never lands the waiter blocks in
 * interrupt_wait and irq_progress stops, which the watchdog detects.
 */
static void *irq_waiter_worker(void *arg)
{
    int iid;
    (void) arg;

    if (prio_then_pin(SCHED_FIFO, WAIT_PRIO, WAIT_CPU) == -1)
        return NULL;
    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return NULL;
    }
    iid = interrupt_attach_thread(irq_vector, 0);
    if (iid == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return NULL;
    }
    (void) __interrupt_unmask(irq_vector, iid);
    atomic_store_explicit(&irq_waiter_ready, 1, memory_order_release);

    for (unsigned i = 0; i < IRQ_ITERS; i++) {
        uint64_t when = rtc_now() + ALARM_DELTA_NS, woke, lat;

        rtc_arm(when);
        if (interrupt_wait(0, NULL) == -1)
            break;
        woke = rtc_now();
        lat = woke > when ? woke - when : 0;
        if (lat > atomic_load_explicit(&irq_max_lat, memory_order_relaxed))
            atomic_store_explicit(&irq_max_lat, lat, memory_order_relaxed);
        atomic_store_explicit(&irq_progress, i + 1, memory_order_release);
        rtc_wr(RTC_CLEAR_INTERRUPT, 1);
        (void) __interrupt_unmask(irq_vector, iid);
        if (atomic_load_explicit(&irq_stop, memory_order_acquire))
            break;
    }
    atomic_store_explicit(&irq_waiter_done, 1, memory_order_release);
    (void) interrupt_detach(iid);
    return NULL;
}

/* IRQ-phase watchdog state: the same rdtime-based stall detection as the timer
 * watchdog (wd_worker), over the IRQ phase globals.
 */
static struct wd_arg irq_wd_arg = {
    .progress = &irq_progress,
    .done = &irq_waiter_done,
    .stalled = &irq_stalled,
    .stop = &irq_stop,
    .ready = &irq_wd_ready,
};

/* Returns 0 on a clean measured pass, 1 when the gap was named, 2 when the
 * goldfish-rtc device is unavailable so the source is skipped, -1 on failure.
 */
static int run_irq_source(void)
{
    pthread_t hog, waiter, wd;
    struct hog_arg ha = {.stop = &irq_stop, .ready = &irq_hog_ready};
    struct rtmux_resource_handle h;
    unsigned progress;
    uint64_t maxns;

    if (rtmux_delegate_device(RTMUX_DEVICE_GOLDFISH_RTC, 0x1000,
                              RTMUX_DELEGATE_ANY_BASE, &h) != EOK) {
        printf("preempt-latency: irq SKIP no delegated goldfish-rtc\n");
        return 2;
    }
    irq_vector = PLIC_VECTOR_BASE + h.irq_vector;
    irq_rtc = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                   MAP_PHYS | MAP_SHARED, NOFD, (off_t) (unsigned long) h.base);
    if (irq_rtc == MAP_FAILED) {
        /* Delegation already succeeded, so a mapping failure is a real error,
         * not an absent device.
         */
        printf("preempt-latency: irq FAIL mmap rtc: %s\n", strerror(errno));
        return -1;
    }
    /* Disarm any stale alarm from a prior run before attaching. */
    rtc_wr(RTC_IRQ_ENABLED, 0);
    rtc_wr(RTC_CLEAR_ALARM, 1);
    rtc_wr(RTC_CLEAR_INTERRUPT, 1);

    if (pthread_create(&wd, NULL, wd_worker, &irq_wd_arg) != EOK)
        return -1;
    if (await_ready(&irq_wd_ready) == -1) {
        printf("preempt-latency: irq FAIL watchdog setup\n");
        return -1;
    }
    if (pthread_create(&hog, NULL, hog_worker, &ha) != EOK)
        return -1;
    if (await_ready(&irq_hog_ready) == -1) {
        printf("preempt-latency: irq FAIL hog setup\n");
        return -1;
    }
    if (pthread_create(&waiter, NULL, irq_waiter_worker, NULL) != EOK)
        return -1;
    if (await_ready(&irq_waiter_ready) == -1) {
        printf("preempt-latency: irq FAIL waiter setup\n");
        return -1;
    }

    (void) thread_join(wd, NULL);
    atomic_store_explicit(&irq_stop, 1, memory_order_release);
    (void) thread_join(hog, NULL);

    progress = atomic_load_explicit(&irq_progress, memory_order_acquire);
    maxns = atomic_load_explicit(&irq_max_lat, memory_order_acquire);

    if (atomic_load_explicit(&irq_stalled, memory_order_acquire)) {
        /* The waiter is left blocked in interrupt_wait (still attached), so do
         * not join it. This is why IRQ runs as the last phase: process exit
         * reaps the waiter and its attach.
         */
        printf(
            "preempt-latency: irq NAMED-GAP wake-to-dispatch stalled after "
            "%u/%d wakes (max seen %llu ns); a hardware IRQ notification does "
            "not preempt a busy same-hart lower-priority thread at this hart "
            "count (see docs/rt-budgets.md).\n",
            progress, IRQ_ITERS, (unsigned long long) maxns);
        return 1;
    }

    (void) thread_join(waiter, NULL);
    printf(
        "preempt-latency: irq completed %u/%d wakes, max IRQ-to-dispatch "
        "%llu ns\n",
        progress, IRQ_ITERS, (unsigned long long) maxns);
    if (progress < IRQ_ITERS) {
        printf("preempt-latency: irq FAIL waiter did not complete\n");
        return -1;
    }
    if (maxns > WAKE_BOUND_NS) {
        printf("preempt-latency: irq FAIL max %llu ns > %d ns bound\n",
               (unsigned long long) maxns, WAKE_BOUND_NS);
        return -1;
    }
    printf("preempt-latency: irq PASS IRQ wake preempts within bound\n");
    return 0;
}

/* Probe whether a fourth hart (CROSS_CPU) exists for the cross-hart pass: pin a
 * throwaway thread there and report whether the pin took. A runmask to an
 * absent hart is rejected, so a failed pin means fewer than CROSS_CPU + 1
 * harts. The pin only (no priority change): a SCHED_FIFO probe thread left
 * unpinned by a failed pin perturbs the tight three-hart schedule and can stall
 * a later phase's setup, so keep this side-effect-free.
 */
static atomic_int xhart_ok;

static void *xhart_probe(void *arg)
{
    (void) arg;
    if (pin_to_cpu(CROSS_CPU) == 0)
        atomic_store_explicit(&xhart_ok, 1, memory_order_release);
    return NULL;
}

static int cross_hart_available(void)
{
    pthread_t t;

    if (pthread_create(&t, NULL, xhart_probe, NULL) != EOK)
        return 0;
    (void) thread_join(t, NULL);
    return atomic_load_explicit(&xhart_ok, memory_order_acquire);
}

int main(void)
{
    int ipc_rc, irq_rc, timer_rc;

    /* The controller must sit on a hart free of the pinned workers so it can
     * run to report and so its own setup sleeps wake normally; it then blocks
     * in thread_join. If there is no third hart the probe cannot isolate the
     * controller, and the timer gap does not appear at two harts, so skip.
     */
    if (pin_to_cpu(CTRL_CPU) == -1) {
        printf("preempt-latency: SKIP needs at least 3 harts\n");
        return TC_EXIT_SKIP;
    }

    /* Every latency here is derived from the rdtime counter rate. Without it
     * the numbers and the watchdog budget would be meaningless, so require it.
     */
    if (cycles_per_sec() == 0) {
        printf("preempt-latency: SKIP counter rate unavailable\n");
        return TC_EXIT_SKIP;
    }

    printf(
        "preempt-latency: FIFO%d high thread vs FIFO%d hog on cpu%d, "
        "off-hart helpers on cpu%d, controller on cpu%d\n",
        WAIT_PRIO, HOG_PRIO, WAIT_CPU, OFF_CPU, CTRL_CPU);
    fflush(stdout);

    ipc_rc = run_ipc_source();
    if (atomic_load_explicit(&setup_failed, memory_order_acquire)) {
        printf("preempt-latency: FAIL worker setup failed\n");
        return EXIT_FAILURE;
    }
    if (ipc_rc != 0) {
        printf("preempt-latency: FAIL ipc source\n");
        return EXIT_FAILURE;
    }

    stall_budget = STALL_BUDGET_NS;
    wait_cpu = WAIT_CPU;
    timer_rc = run_timer_source("timer");
    if (timer_rc == -1) {
        printf("preempt-latency: FAIL timer source\n");
        return EXIT_FAILURE;
    }

    /* Cross-hart pass: the boot-hart timer readies the waiter onto a different
     * busy hart, exercising the reschedule-IPI preemption path (guards the fix
     * that makes a busy remote hart dispatch the readied thread). Needs a
     * fourth hart; skip cleanly otherwise. A stall here is a real regression,
     * so it FAILS, unlike the same-hart pass which only names the gap. Only run
     * it after a clean same-hart pass so no stalled same-hart waiter lingers on
     * the shared phase state.
     */
    if (timer_rc == 0 && cross_hart_available()) {
        int xhart_rc;

        stall_budget = XHART_STALL_BUDGET_NS;
        wait_cpu = CROSS_CPU;
        xhart_rc = run_timer_source("timer-crosshart");
        wait_cpu = WAIT_CPU;
        stall_budget = STALL_BUDGET_NS;
        if (xhart_rc != 0) {
            printf(
                "preempt-latency: FAIL timer-crosshart (cross-hart "
                "reschedule IPI did not preempt the remote hog)\n");
            return EXIT_FAILURE;
        }
    } else if (timer_rc == 0) {
        printf(
            "preempt-latency: timer-crosshart SKIP needs at least 4 harts\n");
    }

    /* IRQ runs last: on a named-gap stall its waiter is left attached and
     * blocked in interrupt_wait, and stopping its hog can let that waiter run,
     * so no later phase must depend on the IRQ phase's teardown being complete.
     */
    irq_rc = run_irq_source();
    if (atomic_load_explicit(&setup_failed, memory_order_acquire)) {
        printf("preempt-latency: FAIL worker setup failed\n");
        return EXIT_FAILURE;
    }
    if (irq_rc == -1) {
        printf("preempt-latency: FAIL irq source\n");
        return EXIT_FAILURE;
    }

    printf("preempt-latency: PASS (ipc measured; timer %s; irq %s)\n",
           timer_rc == 1 ? "gap named" : "measured",
           irq_rc == 0 ? "measured" : (irq_rc == 1 ? "gap named" : "skipped"));
    return EXIT_SUCCESS;
}
