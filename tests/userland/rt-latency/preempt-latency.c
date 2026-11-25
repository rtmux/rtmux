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
 * The timer source runs twice: with the waiter on the hart whose timer fires,
 * and, given a fourth hart, with the waiter and its hog on another hart, so the
 * wake has to cross harts by a reschedule IPI. Each pass runs whatever the
 * other one found.
 *
 * A timer or irq wake that never dispatches is a failure. The watchdog runs on
 * its own hart so it survives a same-hart dispatch failure and can report the
 * stall rather than let the run hang; the controller sits on a third hart and
 * blocks in thread_join, so it never busy-competes with the pinned workers. The
 * ipc source has the same watchdog, so a reply that never arrives or never
 * dispatches fails the probe instead of running into the harness timeout.
 *
 * A wake later than WAKE_BOUND_NS is a failure too, unless the probe was
 * started with --host-audit. Under multi-threaded TCG every hart is a host
 * thread, and a host that deschedules one for tens of milliseconds produces a
 * late wake the guest cannot tell from a missed preemption. With --host-audit
 * the probe reports each late wake with its place on the rdtime clock, writes
 * rdtime beacons the host can timestamp, and leaves the verdict on those
 * samples to scripts/vcpu-steal.py, which fails every one that host scheduling
 * does not account for. Under icount the virtual clock does not see the host
 * at all, so there the bound is checked here, without the audit.
 *
 * Needs at least three harts: waiter/hog, watchdog, controller. With fewer it
 * skips.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdbool.h>
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
#include <sys/wait.h>

#include <arch/plic-vector.h>

#include "../outcome.h"
#include "rtc-probe.h"

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
 * a grossly late wake.
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

/* The delegated goldfish-rtc (rtc-probe.h); its base is NULL without one. The
 * irq source needs its alarm, and the host audit its beacons.
 */
static struct rtc_probe rtc;
static int rtc_vector;

/* Host audit of late wakes (--host-audit, see the file comment). */
static int host_audit;

/* Self-test of the detector (--inject-stall MS, debug-inject kernels only):
 * run the cross-hart timer pass alone, longer, and part way through have the
 * kernel hold CROSS_CPU with interrupts off for this many milliseconds. The
 * pass must then report a late wake.
 */
static unsigned inject_stall_ms;
static atomic_int inject_result; /* 1 the hart was stalled, -1 it was not */

/* Self-test of the ipc watchdog (--drop-reply): the server withholds the reply
 * to one request after the warm-up, and the ipc pass must report the stall.
 */
static bool drop_reply;
#define DROP_REPLY_AT (IPC_WARMUP + 8) /* request whose reply is withheld */
#define INJECT_ITERS 1000        /* wakes of the injected pass: one second */
#define INJECT_AFTER 20          /* wakes to let pass before the stall */

/* Every wake later than the bound, kept for the report after the phase: the
 * waiter must not print between wakes.
 */
struct late_wake {
    uint64_t late_ns;
    /* rdtime just before and just after the clock read that ended the
     * measurement: the wake was measured somewhere between the two.
     */
    uint64_t ticks_before, ticks_after;
    unsigned idx;
};
#define LATE_MAX 256
static struct late_wake late_wakes[LATE_MAX];
static unsigned late_count;   /* written by the one waiter of the phase */
static unsigned late_dropped; /* late wakes that did not fit late_wakes */
static unsigned unaudited; /* late wakes left to the host audit, all phases */

/* Written to CLEAR_ALARM, in this order, before the first beacon, so the host
 * can tell the beacons from earlier writes to the register ("RTMX", "BCNS").
 */
#define BEACON_START_0 0x52544d58U
#define BEACON_START_1 0x42434e53U
/* Wait after the markers, before the first phase: several slow polls of the
 * host sampler (SLOW_PERIOD_S in scripts/vcpu-steal.py).
 */
#define AUDIT_SETTLE_MS 250

/* Tell the host what rdtime is now: QEMU can trace each write with a host
 * timestamp, which places the probe's rdtime values on the host clock. The
 * register ignores the value it is given, and clearing an alarm that has
 * already fired, or was never armed, changes nothing. Two writes, each with a
 * fresh reading: the first write then lies between two known readings, which
 * bounds the clock offset from both sides.
 */
static void host_beacon(void)
{
    if (!host_audit || !rtc.base)
        return;
    rtc_wr(&rtc, RTC_CLEAR_ALARM, (uint32_t) read_time());
    rtc_wr(&rtc, RTC_CLEAR_ALARM, (uint32_t) read_time());
}

/* Called by the waiter after each wake, once it has taken its measurement. */
static void wake_measured(unsigned idx,
                          uint64_t late_ns,
                          uint64_t ticks_before,
                          uint64_t ticks_after)
{
    host_beacon();
    if (late_ns <= WAKE_BOUND_NS)
        return;
    if (late_count == LATE_MAX) {
        late_dropped++;
        return;
    }
    late_wakes[late_count] = (struct late_wake){
        .late_ns = late_ns,
        .ticks_before = ticks_before,
        .ticks_after = ticks_after,
        .idx = idx,
    };
    late_count++;
}

/* Forget the late wakes of an earlier phase. */
static void late_reset(void)
{
    late_count = 0;
    late_dropped = 0;
}

/* Judge a completed phase whose waiter has been joined: within the bound, or
 * late. WAIT_CPU takes the wake's interrupt and wait_cpu runs the waiter; with
 * via_main the wake also needs QEMU's main loop, which delivers timer and
 * device interrupts. The host audit charges only those.
 *
 * Returns 0 when the phase passed or its late wakes went to the host audit, -1
 * when it failed.
 */
static int judge_latency(const char *label,
                         const char *what,
                         uint64_t maxns,
                         unsigned wakes,
                         int via_main)
{
    for (unsigned i = 0; i < late_count; i++)
        printf(
            "preempt-latency: %s over-bound i=%u late_ns=%llu "
            "wake_ticks=%llu-%llu fire_cpu=%u target_cpu=%u main_loop=%d\n",
            label, late_wakes[i].idx,
            (unsigned long long) late_wakes[i].late_ns,
            (unsigned long long) late_wakes[i].ticks_before,
            (unsigned long long) late_wakes[i].ticks_after, WAIT_CPU, wait_cpu,
            via_main);
    if (maxns <= WAKE_BOUND_NS) {
        printf("preempt-latency: %s PASS %s max %llu ns within bound\n", label,
               what, (unsigned long long) maxns);
        return 0;
    }
    /* A late wake that was not recorded cannot be audited. */
    if (!host_audit || late_dropped) {
        printf("preempt-latency: %s FAIL max %llu ns > %d ns bound\n", label,
               (unsigned long long) maxns, WAKE_BOUND_NS);
        return -1;
    }
    printf(
        "preempt-latency: %s UNAUDITED %u of %u wakes over the %d ns bound "
        "(max %llu ns), valid only if the host audit clears each\n",
        label, late_count, wakes, WAKE_BOUND_NS, (unsigned long long) maxns);
    unaudited += late_count;
    return 0;
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
 * (see pi-inversion-xfail), so this measures a working reply-to-dispatch. A
 * watchdog on OFF_CPU still bounds it: a reply that is lost, or that never
 * dispatches the client, leaves the client blocked in msg_send and stops its
 * progress count, and the pass fails when the watchdog says so.
 */

/* State a watchdog observes for one phase. The ipc, timer and IRQ phases differ
 * only in which globals they watch, so all drive the same wd_worker through
 * this.
 */
struct wd_arg {
    atomic_uint *progress; /* iterations the waiter has completed */
    atomic_int *done;      /* waiter finished */
    atomic_int *stalled;   /* watchdog sets on a declared stall */
    atomic_int *stop;      /* watchdog sets to stop the hog on a stall */
    atomic_int *ready;     /* watchdog publishes its own readiness here */
};

static void *wd_worker(void *arg);

static atomic_int ipc_srv_ready, ipc_client_done;
static atomic_uint ipc_progress; /* client steps: connect retries and sends */
static atomic_uint ipc_sends;    /* sends the client has completed */
static atomic_int ipc_client_exited, ipc_stalled, ipc_wd_stop, ipc_wd_ready;
static struct wd_arg ipc_wd_arg = {
    .progress = &ipc_progress,
    .done = &ipc_client_exited,
    .stalled = &ipc_stalled,
    .stop = &ipc_wd_stop, /* no hog to stop in this phase */
    .ready = &ipc_wd_ready,
};
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
    unsigned served = 0;
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
        if (drop_reply && served++ == DROP_REPLY_AT)
            continue; /* the client stays reply-blocked on rcvid */
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
static void ipc_client_run(void)
{
    static char sbuf[256], rbuf[256];
    uint64_t rate = cycles_per_sec();
    int coid = -1;

    if (prio_then_pin(SCHED_FIFO, WAIT_PRIO, WAIT_CPU) == -1)
        return;

    for (int tries = 0; tries < 1000; tries++) {
        coid = connect_attach(0, getpid(), ipc_chid, RTMUX_SIDE_CHANNEL, 0);
        if (coid != -1)
            break;
        atomic_fetch_add_explicit(&ipc_progress, 1, memory_order_release);
        if (errno != ESRCH && errno != ENOENT && errno != EAGAIN) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return;
        }
        (void) sleep_ms(1);
    }
    if (coid == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return;
    }

    memset(sbuf, 0xA5, sizeof sbuf);
    sbuf[0] = OP_ECHO;
    for (int i = 0; i < IPC_WARMUP + IPC_ITERS; i++) {
        uint64_t wake, reply, lat;

        if (msg_send(coid, sbuf, sizeof sbuf, rbuf, sizeof rbuf) == -1) {
            atomic_store_explicit(&setup_failed, 1, memory_order_release);
            return;
        }
        wake = read_time();
        reply = atomic_load_explicit(&ipc_reply_ts, memory_order_acquire);
        lat = wake > reply ? wake - reply : 0;

        /* The controller reads ipc_samples only after joining this thread, so
         * the join is the ordering barrier and no atomic is needed here.
         */
        if (i >= IPC_WARMUP) {
            ipc_samples[i - IPC_WARMUP] = lat;
            wake_measured((unsigned) (i - IPC_WARMUP), ticks_to_ns(lat, rate),
                          wake, wake);
        }
        atomic_fetch_add_explicit(&ipc_sends, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&ipc_progress, 1, memory_order_release);
    }

    sbuf[0] = OP_STOP;
    (void) msg_send(coid, sbuf, sizeof sbuf, rbuf, sizeof rbuf);
    atomic_store_explicit(&ipc_client_done, 1, memory_order_release);
}

/* The client thread: tells the watchdog it has finished, whichever way. */
static void *ipc_client(void *arg)
{
    (void) arg;
    ipc_client_run();
    atomic_store_explicit(&ipc_client_exited, 1, memory_order_release);
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

/* Returns 0 on a clean measured pass, -1 on failure. After a stall the client
 * is still reply-blocked and the server may still hold its request, so neither
 * is joined; the caller exits.
 */
static int run_ipc_source(void)
{
    pthread_t srv, client, wd;
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

    late_reset();
    if (pthread_create(&srv, NULL, ipc_server, NULL) != EOK)
        return -1;
    if (await_ready(&ipc_srv_ready) == -1) {
        printf("preempt-latency: ipc FAIL server setup\n");
        release_ipc_server();
        (void) thread_join(srv, NULL);
        return -1;
    }
    if (pthread_create(&wd, NULL, wd_worker, &ipc_wd_arg) != EOK) {
        release_ipc_server();
        (void) thread_join(srv, NULL);
        return -1;
    }
    if (await_ready(&ipc_wd_ready) == -1) {
        printf("preempt-latency: ipc FAIL watchdog setup\n");
        atomic_store_explicit(&ipc_client_exited, 1, memory_order_release);
        (void) thread_join(wd, NULL);
        release_ipc_server();
        (void) thread_join(srv, NULL);
        return -1;
    }
    if (pthread_create(&client, NULL, ipc_client, NULL) != EOK) {
        atomic_store_explicit(&ipc_client_exited, 1, memory_order_release);
        (void) thread_join(wd, NULL);
        release_ipc_server();
        (void) thread_join(srv, NULL);
        return -1;
    }

    (void) thread_join(wd, NULL);
    if (atomic_load_explicit(&ipc_stalled, memory_order_acquire)) {
        printf("preempt-latency: ipc FAIL client stalled in msg_send after "
               "%u/%d sends; a reply was lost or never dispatched it\n",
               atomic_load_explicit(&ipc_sends, memory_order_acquire),
               IPC_WARMUP + IPC_ITERS);
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
    return judge_latency("ipc", "reply-to-dispatch", maxns, IPC_ITERS, 0);
}

/* timer source */

static unsigned timer_iters = TIMER_ITERS;
static atomic_int timer_stop;
static atomic_int timer_hog_ready, waiter_ready, wd_ready;
static atomic_uint waiter_progress; /* iterations the waiter has completed */
static atomic_int waiter_done;
static atomic_int stalled;
static atomic_uint_least64_t
    timer_max_lat; /* max wake-to-dispatch lateness, ns */

/* Whether the --inject-stall self-test has yet to request its stall. */
static int inject_pending(void)
{
    return inject_stall_ms &&
           atomic_load_explicit(&inject_result, memory_order_acquire) == 0 &&
           !atomic_load_explicit(&timer_stop, memory_order_acquire);
}

/* The waiter sleeps to an absolute deadline; a working timer wake preempts the
 * hog so the waiter measures a small gap. If the wake never lands the waiter
 * blocks forever in clock_nanosleep and waiter_progress stops advancing, which
 * the watchdog on the other hart detects.
 */
static void *waiter_worker(void *arg)
{
    struct timespec deadline, now;
    (void) arg;

    if (prio_then_pin(SCHED_FIFO, WAIT_PRIO, wait_cpu) == -1)
        return NULL;
    atomic_store_explicit(&waiter_ready, 1, memory_order_release);

    clock_gettime(CLOCK_MONOTONIC, &deadline);
    now = deadline;
    /* The injected pass also runs until the stall request has been made: the
     * stall must find the waiter still sleeping and waking.
     */
    for (unsigned i = 0; i < timer_iters || inject_pending(); i++) {
        uint64_t late, before;

        /* When the next deadline has already passed, after a late wake or a
         * slow beacon, start again from now: one long delay is then one late
         * sample and not a run of them.
         */
        if (ts_ns(&now) > ts_ns(&deadline) + SLEEP_NS)
            deadline = now;
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
        before = read_time();
        clock_gettime(CLOCK_MONOTONIC, &now);
        late =
            ts_ns(&now) > ts_ns(&deadline) ? ts_ns(&now) - ts_ns(&deadline) : 0;
        wake_measured(i, late, before, read_time());
        /* The beacons are device writes, which can take long. */
        if (host_audit)
            clock_gettime(CLOCK_MONOTONIC, &now);
        if (late > atomic_load_explicit(&timer_max_lat, memory_order_relaxed))
            atomic_store_explicit(&timer_max_lat, late, memory_order_relaxed);
        atomic_store_explicit(&waiter_progress, i + 1, memory_order_release);
        if (atomic_load_explicit(&timer_stop, memory_order_acquire))
            break;
    }
    atomic_store_explicit(&waiter_done, 1, memory_order_release);
    return NULL;
}

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

/* Returns 0 on a completed pass within the bound (or with its late wakes left
 * to the host audit), -1 on failure. After a stall the waiter is still blocked
 * and owns the phase state, so no further timer pass may run.
 */
static int run_timer_source(const char *label)
{
    /* Static: a hog left behind by a failed setup still reads it. */
    static struct hog_arg ha = {.stop = &timer_stop, .ready = &timer_hog_ready};
    pthread_t hog, waiter, wd;
    unsigned progress;
    uint64_t maxns;

    /* Re-runnable: main drives a same-hart pass and a cross-hart pass, so
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
    late_reset();

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
            "preempt-latency: %s FAIL wake-to-dispatch stalled after %u/%u "
            "wakes (max seen %llu ns); a nanosleep wheel timer did not "
            "preempt a busy lower-priority thread on cpu%u\n",
            label, progress, timer_iters, (unsigned long long) maxns, wait_cpu);
        return -1;
    }

    (void) thread_join(waiter, NULL);
    printf(
        "preempt-latency: %s completed %u/%u wakes, max wake-to-dispatch "
        "%llu ns\n",
        label, progress, timer_iters, (unsigned long long) maxns);
    if (progress < timer_iters) {
        printf("preempt-latency: %s FAIL waiter did not complete\n", label);
        return -1;
    }
    return judge_latency(label, "wake-to-dispatch", maxns, timer_iters, 1);
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

#define ALARM_DELTA_NS 1000000ULL /* 1 ms ahead */
#define IRQ_ITERS 60

static atomic_int irq_stop;
static atomic_int irq_hog_ready, irq_waiter_ready, irq_wd_ready;
static atomic_uint irq_progress;
static atomic_int irq_waiter_done;
static atomic_int irq_stalled;
static atomic_uint_least64_t irq_max_lat; /* ns */

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
    iid = interrupt_attach_thread(rtc_vector, 0);
    if (iid == -1) {
        atomic_store_explicit(&setup_failed, 1, memory_order_release);
        return NULL;
    }
    (void) __interrupt_unmask(rtc_vector, iid);
    atomic_store_explicit(&irq_waiter_ready, 1, memory_order_release);

    for (unsigned i = 0; i < IRQ_ITERS; i++) {
        uint64_t when = rtc_now(&rtc) + ALARM_DELTA_NS, woke, lat, before;

        rtc_arm(&rtc, when, 1);
        if (interrupt_wait(0, NULL) == -1)
            break;
        before = read_time();
        woke = rtc_now(&rtc);
        lat = woke > when ? woke - when : 0;
        wake_measured(i, lat, before, read_time());
        if (lat > atomic_load_explicit(&irq_max_lat, memory_order_relaxed))
            atomic_store_explicit(&irq_max_lat, lat, memory_order_relaxed);
        atomic_store_explicit(&irq_progress, i + 1, memory_order_release);
        rtc_clear(&rtc);
        (void) __interrupt_unmask(rtc_vector, iid);
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

/* Take the goldfish-rtc, once, before any phase.
 *
 * Returns 0 with rtc.base set, 0 with rtc.base NULL when no device is
 * delegated, -1 when the delegated device cannot be mapped.
 */
static int rtc_open(void)
{
    struct rtmux_resource_handle h;
    void *regs;

    if (rtmux_delegate_device(RTMUX_DEVICE_GOLDFISH_RTC, 0x1000,
                              RTMUX_DELEGATE_ANY_BASE, &h) != EOK)
        return 0;
    regs = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
                MAP_PHYS | MAP_SHARED, NOFD, (off_t) (unsigned long) h.base);
    if (regs == MAP_FAILED) {
        /* Delegation already succeeded, so a mapping failure is a real error,
         * not an absent device.
         */
        printf("preempt-latency: FAIL mmap rtc: %s\n", strerror(errno));
        return -1;
    }
    rtc_vector = PLIC_VECTOR_BASE + h.irq_vector;
    rtc.base = regs;
    /* Disarm any stale alarm from a prior run, here and not in the irq phase:
     * after the two marker writes the host takes every write to CLEAR_ALARM
     * for a beacon.
     */
    rtc_reset(&rtc);
    if (host_audit) {
        rtc_wr(&rtc, RTC_CLEAR_ALARM, BEACON_START_0);
        rtc_wr(&rtc, RTC_CLEAR_ALARM, BEACON_START_1);
        /* The host places each sample by its distance from the first beacon,
         * so one must precede the first sample. The host also polls QEMU's
         * threads slowly until it sees these writes: give it time to switch
         * to the fast polls, or the first wakes have no wait proven for them.
         */
        host_beacon();
        (void) sleep_ms(AUDIT_SETTLE_MS);
    }
    return 0;
}

/* Returns 0 on a completed pass within the bound (or with its late wakes left
 * to the host audit), 2 when the goldfish-rtc device is unavailable so the
 * source is skipped, -1 on failure.
 */
static int run_irq_source(void)
{
    /* Static: a hog left behind by a failed setup still reads it. */
    static struct hog_arg ha = {.stop = &irq_stop, .ready = &irq_hog_ready};
    pthread_t hog, waiter, wd;
    unsigned progress;
    uint64_t maxns;

    if (!rtc.base) {
        printf("preempt-latency: irq SKIP no delegated goldfish-rtc\n");
        return 2;
    }
    late_reset();

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
            "preempt-latency: irq FAIL wake-to-dispatch stalled after %u/%d "
            "wakes (max seen %llu ns); a hardware IRQ notification did not "
            "preempt a busy same-hart lower-priority thread\n",
            progress, IRQ_ITERS, (unsigned long long) maxns);
        return -1;
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
    return judge_latency("irq", "IRQ-to-dispatch", maxns, IRQ_ITERS, 1);
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

extern char **environ;

/* Stall CROSS_CPU once the cross-hart waiter is under way. This thread and the
 * /bin/inject it spawns inherit the controller's pin, so the stall request
 * never comes from the hart it names, which the kernel would refuse.
 */
static void *stall_injector(void *arg)
{
    char hart[12], ms[12];
    char *argv[] = {"inject", "spin-irqoff", hart, ms, NULL};
    pid_t pid;
    int status;
    (void) arg;

    while (atomic_load_explicit(&waiter_progress, memory_order_acquire) <
           INJECT_AFTER) {
        /* A stall injected after the waiter has finished would prove nothing.
         */
        if (atomic_load_explicit(&waiter_done, memory_order_acquire) ||
            atomic_load_explicit(&timer_stop, memory_order_acquire)) {
            atomic_store_explicit(&inject_result, -1, memory_order_release);
            return NULL;
        }
        (void) sleep_ms(1);
    }
    snprintf(hart, sizeof hart, "%u", (unsigned) CROSS_CPU);
    snprintf(ms, sizeof ms, "%u", inject_stall_ms);
    if (posix_spawn(&pid, "/bin/inject", NULL, NULL, argv, environ) == 0 &&
        waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0)
        atomic_store_explicit(&inject_result, 1, memory_order_release);
    else
        atomic_store_explicit(&inject_result, -1, memory_order_release);
    return NULL;
}

/* Returns the exit status of the --inject-stall self-test: success only when
 * the stall was injected and the pass reported it as a late wake.
 */
static int run_inject_selftest(void)
{
    pthread_t injector;
    uint64_t maxns;
    int rc;

    if (!cross_hart_available()) {
        printf("preempt-latency: inject FAIL needs at least 4 harts\n");
        return EXIT_FAILURE;
    }
    timer_iters = INJECT_ITERS;
    stall_budget = XHART_STALL_BUDGET_NS;
    wait_cpu = CROSS_CPU;
    if (pthread_create(&injector, NULL, stall_injector, NULL) != EOK)
        return EXIT_FAILURE;
    rc = run_timer_source("timer-crosshart");
    /* A pass that failed in setup never got far enough to release it. */
    atomic_store_explicit(&timer_stop, 1, memory_order_release);
    (void) thread_join(injector, NULL);
    maxns = atomic_load_explicit(&timer_max_lat, memory_order_acquire);

    if (atomic_load_explicit(&inject_result, memory_order_acquire) != 1) {
        printf(
            "preempt-latency: inject FAIL could not stall cpu%d (needs a "
            "CONFIG_DEBUG_INJECT kernel)\n",
            CROSS_CPU);
        return EXIT_FAILURE;
    }
    /* Under the host audit the late wakes are reported and left to the host,
     * which must refuse them: the hart was running, not descheduled.
     */
    if (host_audit && rc == 0 && unaudited) {
        printf(
            "preempt-latency: AUDIT-REQUIRED %u wakes over the bound after "
            "a %u ms injected stall; the host audit must refuse them\n",
            unaudited, inject_stall_ms);
        return EXIT_SUCCESS;
    }
    if (rc != -1 || maxns < (uint64_t) inject_stall_ms * 1000000ULL / 2) {
        printf(
            "preempt-latency: inject FAIL a %u ms stall of cpu%d went "
            "unreported (max wake-to-dispatch %llu ns)\n",
            inject_stall_ms, CROSS_CPU, (unsigned long long) maxns);
        return EXIT_FAILURE;
    }
    printf(
        "preempt-latency: inject PASS a %u ms stall of cpu%d was reported "
        "as a %llu ns late wake\n",
        inject_stall_ms, CROSS_CPU, (unsigned long long) maxns);
    return EXIT_SUCCESS;
}

int main(int argc, char **argv)
{
    int ipc_rc, irq_rc, timer_rc;
    bool xhart_ran = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--host-audit") == 0) {
            host_audit = 1;
        } else if (strcmp(argv[i], "--inject-stall") == 0 && i + 1 < argc) {
            inject_stall_ms = (unsigned) strtoul(argv[++i], NULL, 0);
        } else if (strcmp(argv[i], "--drop-reply") == 0) {
            drop_reply = true;
        } else {
            printf(
                "usage: preempt-latency [--host-audit] "
                "[--inject-stall MS | --drop-reply]\n");
            return EXIT_FAILURE;
        }
    }

    /* The controller must sit on a hart free of the pinned workers so it can
     * run to report and so its own setup sleeps wake normally; it then blocks
     * in thread_join. If there is no third hart the probe cannot isolate the
     * controller, so skip.
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

    if (rtc_open() == -1)
        return EXIT_FAILURE;

    printf(
        "preempt-latency: FIFO%d high thread vs FIFO%d hog on cpu%d, "
        "off-hart helpers on cpu%d, controller on cpu%d%s\n",
        WAIT_PRIO, HOG_PRIO, WAIT_CPU, OFF_CPU, CTRL_CPU,
        host_audit ? ", late wakes left to the host audit" : "");
    fflush(stdout);

    if (inject_stall_ms)
        return run_inject_selftest();
    if (drop_reply) {
        if (run_ipc_source() == -1 &&
            atomic_load_explicit(&ipc_stalled, memory_order_acquire)) {
            printf("preempt-latency: drop-reply PASS the withheld reply "
                   "failed the ipc pass\n");
            return EXIT_SUCCESS;
        }
        printf("preempt-latency: drop-reply FAIL the withheld reply went "
               "unreported\n");
        return EXIT_FAILURE;
    }

    ipc_rc = run_ipc_source();
    if (atomic_load_explicit(&setup_failed, memory_order_acquire)) {
        printf("preempt-latency: FAIL worker setup failed\n");
        return EXIT_FAILURE;
    }
    if (ipc_rc != 0) {
        printf("preempt-latency: FAIL ipc source\n");
        return EXIT_FAILURE;
    }

    /* Cross-hart pass first: the boot-hart timer readies the waiter onto a
     * different busy hart, exercising the reschedule-IPI preemption path
     * (guards the fix that makes a busy remote hart dispatch the readied
     * thread). It needs a fourth hart. It does not depend on the same-hart
     * pass, and runs first because a stalled pass leaves its waiter blocked on
     * the shared phase state, which ends the timer passes.
     */
    if (cross_hart_available()) {
        stall_budget = XHART_STALL_BUDGET_NS;
        wait_cpu = CROSS_CPU;
        xhart_ran = true;
        if (run_timer_source("timer-crosshart") != 0) {
            printf("preempt-latency: FAIL timer-crosshart source\n");
            return EXIT_FAILURE;
        }
    } else {
        printf(
            "preempt-latency: timer-crosshart SKIP needs at least 4 harts\n");
    }

    stall_budget = STALL_BUDGET_NS;
    wait_cpu = WAIT_CPU;
    timer_rc = run_timer_source("timer");
    if (atomic_load_explicit(&setup_failed, memory_order_acquire)) {
        printf("preempt-latency: FAIL worker setup failed\n");
        return EXIT_FAILURE;
    }
    if (timer_rc != 0) {
        printf("preempt-latency: FAIL timer source\n");
        return EXIT_FAILURE;
    }

    /* IRQ runs last: on a stall its waiter is left attached and blocked in
     * interrupt_wait, and stopping its hog can let that waiter run, so no
     * later phase must depend on the IRQ phase's teardown being complete.
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

    if (unaudited) {
        printf(
            "preempt-latency: AUDIT-REQUIRED %u wakes over the bound; this is "
            "a pass only if scripts/vcpu-steal.py clears every one\n",
            unaudited);
        return EXIT_SUCCESS;
    }
    printf(
        "preempt-latency: PASS (ipc measured; timer measured; "
        "timer-crosshart %s; irq %s)\n",
        xhart_ran ? "measured" : "skipped",
        irq_rc == 0 ? "measured" : "skipped");
    return EXIT_SUCCESS;
}
