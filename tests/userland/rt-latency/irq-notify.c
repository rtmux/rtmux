/*
 * irq-notify - section 15 unified-wait fixture: drive a real hardware IRQ two
 * ways and compare.
 *
 * A single MsgReceive-style loop can service messages, pulses, timers, and a
 * channel-routed IRQ together: interrupt_attach_event registers an IRQ as a
 * SIGEV_NOTIFICATION sigevent, and on fire the kernel delivers a notification
 * onto the target channel send_queue exactly like a pulse. This probe proves
 * that path works with a genuine hardware interrupt (not a synthetic call) and
 * measures its wake latency against the dedicated interrupt_wait/STATE_INTR
 * path, so the ergonomic cost of the unified wait is measured, not assumed.
 *
 * The IRQ source is the QEMU virt goldfish-rtc alarm (delegated by the
 * virtio-mmio server as RTMUX_DEVICE_GOLDFISH_RTC). It is isolated from the
 * OpenSBI firmware console, and the kernel reads it only once at boot for
 * wall-clock time, so a userland owner of its alarm IRQ does not collide with
 * anything. The alarm is a deterministic self-trigger: arm it a fixed delta in
 * the future and it raises IRQ 11 at that RTC time.
 *
 * Pass criteria:
 *   - channel path: the IRQ arrives as a notification with the exact code/value
 *     it was attached with, and a higher-priority IRQ notification is dequeued
 *     ahead of a lower-priority pulse already queued on the same channel
 *     (priority order honored);
 *   - dedicated path: interrupt_wait wakes on the same IRQ;
 *   - both wake latencies are reported. Absolute latency under QEMU/TCG is
 *     inflated by host descheduling, so it is reported, not gated; the durable
 *     signal is that the channel path carries an extra receive-loop hop over the
 *     direct IST wake.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdint.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/siginfo.h>
#include <sys/iomsg.h>
#include <sys/virtio-mmio-msg.h>

#include <arch/plic-vector.h>

#include "rtc-probe.h"

#define IRQ_CODE 42        /* user notification code for the RTC IRQ */
#define IRQ_VALUE 0x1234   /* user notification value */
#define IRQ_PRIORITY 60    /* high: the IRQ jumps ahead of the low pulse */
#define LOW_CODE 7         /* a lower-priority pulse queued on the channel */
#define LOW_VALUE 0x5678
#define LOW_PRIORITY 5
#define ALARM_DELTA_NS 1000000ULL /* 1 ms into the future */
#define LAT_ITERS 50
#define EXIT_ROUNDS 100
#define EXIT_GUARD_NS 500000000ULL /* a lost alarm fails the round, not hangs */
#define EXIT_WAIT_NS 20000000ULL /* bounded "no second wake" wait: 20 ms */

static struct rtc_probe probe; /* shared RTC state (rtc-probe.h) */

/* Delegate the goldfish-rtc MMIO + IRQ from the virtio-mmio resource server,
 * the same delegated-capability path dev-serial-8250 uses for the UART.
 *
 * Returns the physical base in *phys and the raw PLIC IRQ number in *irq.
 */
static int rtc_delegate(unsigned long *phys, int *irq)
{
    struct rtmux_resource_handle h;
    int rc = rtmux_delegate_device(RTMUX_DEVICE_GOLDFISH_RTC, 0x1000,
                                   RTMUX_DELEGATE_ANY_BASE, &h);

    if (rc != EOK)
        return rc;
    *phys = (unsigned long) h.base;
    *irq = h.irq_vector;
    return EOK;
}

/* Per-path result of the attempt loop. */
struct path_result {
    uint64_t lat[LAT_ITERS]; /* wake - alarm of every `ok` attempt */
    unsigned nok;
    unsigned counts[3];       /* ok / early / timeout, this path only */
};

static int suppress_one;           /* --suppress-one */
static int suppress_at = -1;       /* global attempt index to suppress */
static unsigned suppress_timeouts; /* timeouts seen on the suppressed attempt */
static unsigned suppress_window_ok;
static uint64_t suppress_elapsed_ns;

struct chan_ctx {
    struct rtc_irq_ctx irq; /* first: rtc_irq_prepare takes the ctx as-is */
    int chid;
};

static int chan_wait(void *ctx)
{
    struct chan_ctx *c = ctx;
    struct _notification n;

    if (rtc_probe_deadline(RTMUX_TIMEOUT_RECEIVE) == -1)
        return errno;
    memset(&n, 0, sizeof n);
    if (msg_receive_notification(c->chid, &n, sizeof n, NULL) != 0)
        return errno ? errno : EIO;
    if (n.code != IRQ_CODE || n.value.sival_int != IRQ_VALUE) {
        fprintf(stderr, "irq-notify: chan: unexpected notification code=%d\n",
                n.code);
        return EPROTO;
    }
    return 0;
}

static const struct rtc_probe_ops chan_ops = {rtc_irq_prepare, chan_wait};
static const struct rtc_probe_ops ist_ops = {rtc_irq_prepare, rtc_ist_wait};

/* LAT_ITERS attempts on one wait path.  None is retaken; a bad one is counted
 * and the run is judged on the totals in main().  suppress_at names the one
 * global attempt (across both paths) whose IRQ is gated off.
 */
static int run_attempts(const struct rtc_probe_ops *ops,
                        void *ctx,
                        const char *path,
                        struct path_result *res)
{
    for (int i = 0; i < LAT_ITERS; i++) {
        struct rtc_attempt a;
        int sup = suppress_one && (int) probe.attempts == suppress_at;

        if (rtc_probe_attempt(&probe, ops, ctx, sup, "irq-notify", path, &a) !=
            0)
            return -1;
        res->counts[a.outcome]++;
        if (a.outcome == RTC_OUTCOME_OK)
            res->lat[res->nok++] = a.wake_ns - a.alarm_ns;
        if (sup && a.outcome == RTC_OUTCOME_TIMEOUT) {
            uint64_t el = a.wake_ns - a.post_arm_ns;

            suppress_timeouts++;
            suppress_elapsed_ns = el;
            if (el >= RTC_PROBE_SUPPRESS_MIN_NS &&
                el <= RTC_PROBE_SUPPRESS_MAX_NS)
                suppress_window_ok++;
        }
    }
    return 0;
}

/* Channel path: attach the IRQ as a SIGEV_NOTIFICATION on a channel, prove the
 * unified receive works and that a higher-priority IRQ notification is dequeued
 * ahead of a lower-priority pulse already sitting on the same channel.
 */
static int test_channel_priority(int vector, struct path_result *res)
{
    struct sigevent ev;
    struct _notification n;
    struct chan_ctx cc;
    int chid, coid, id;

    chid = channel_create(0);
    if (chid == -1) {
        fprintf(stderr, "irq-notify: channel_create: %s\n", strerror(errno));
        return -1;
    }
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        fprintf(stderr, "irq-notify: connect_attach: %s\n", strerror(errno));
        return -1;
    }

    SIGEV_NOTIFICATION_INIT(&ev, coid, IRQ_PRIORITY, IRQ_CODE, IRQ_VALUE);
    id = __interrupt_attach_event(vector, &ev, 0);
    if (id == -1) {
        fprintf(stderr, "irq-notify: attach_event: %s\n", strerror(errno));
        return -1;
    }

    /* Priority order: queue a low-priority pulse, then let the higher-priority
     * IRQ fire while nothing is receiving, so both sit on send_queue. The IRQ
     * notification must come out first.
     */
    if (msg_send_notification(coid, LOW_PRIORITY, LOW_CODE, LOW_VALUE) != EOK) {
        fprintf(stderr, "irq-notify: msg_send_notification: %s\n",
                strerror(errno));
        return -1;
    }
    {
        /* Wait until the RTC has actually passed the alarm deadline (so the
         * hardware IRQ has fired), using bounded short blocking sleeps rather
         * than one fixed guess: a fixed sleep could return before delivery and
         * spuriously receive the low pulse first, and a busy-spin could be
         * starved. Anchoring on the RTC advancing makes ordering deterministic;
         * the bound stops a misbehaving RTC from hanging the probe.
         */
        uint64_t when = rtc_now(&probe) + ALARM_DELTA_NS;
        struct timespec nap = {.tv_sec = 0, .tv_nsec = ALARM_DELTA_NS};
        int spins = 0;

        rtc_arm(&probe, when, 1);
        while (rtc_now(&probe) <= when && spins++ < 1000)
            (void) nanosleep(&nap, NULL);
        /* One more short sleep so the delivered notification has settled onto
         * send_queue before the ordered receive below.
         */
        (void) nanosleep(&nap, NULL);
    }

    memset(&n, 0, sizeof n);
    if (rtc_probe_deadline(RTMUX_TIMEOUT_RECEIVE) == -1 ||
        msg_receive_notification(chid, &n, sizeof n, NULL) != 0) {
        fprintf(stderr, "irq-notify: receive #1: %s\n", strerror(errno));
        return -1;
    }
    if (n.type != _NOTIFICATION_TYPE || n.subtype != _NOTIFICATION_SUBTYPE ||
        n.code != IRQ_CODE || n.value.sival_int != IRQ_VALUE || n.scoid == 0) {
        fprintf(stderr,
                "irq-notify: FAIL priority order: first out was type=%d code=%d "
                "val=%d scoid=%d (expected well-formed IRQ code=%d val=%d ahead "
                "of the low pulse)\n",
                n.type, n.code, n.value.sival_int, n.scoid, IRQ_CODE, IRQ_VALUE);
        return -1;
    }
    rtc_clear(&probe);
    (void) __interrupt_unmask(vector, id);

    memset(&n, 0, sizeof n);
    if (rtc_probe_deadline(RTMUX_TIMEOUT_RECEIVE) == -1 ||
        msg_receive_notification(chid, &n, sizeof n, NULL) != 0 ||
        n.code != LOW_CODE) {
        fprintf(stderr, "irq-notify: FAIL low pulse not second (code=%d)\n",
                n.code);
        return -1;
    }

    /* Latency loop: every attempt is one seeded alarm, one bounded receive and
     * exactly one outcome (rtc-probe.h).
     */
    cc.chid = chid;
    cc.irq.vector = vector;
    cc.irq.id = id;
    if (run_attempts(&chan_ops, &cc, "chan", res) != 0)
        return -1;

    (void) interrupt_detach(id);
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
    return 0;
}

/* Dedicated path: the classic IST model, interrupt_attach_thread then block in
 * interrupt_wait. Same alarm, same latency metric, for comparison.
 */
static int test_dedicated(int vector, struct path_result *res)
{
    struct rtc_irq_ctx ic;
    int iid;

    iid = interrupt_attach_thread(vector, 0);
    if (iid == -1) {
        fprintf(stderr, "irq-notify: attach_thread: %s\n", strerror(errno));
        return -1;
    }
    ic.vector = vector;
    ic.id = iid;
    if (run_attempts(&ist_ops, &ic, "ist", res) != 0) {
        (void) interrupt_detach(iid);
        return -1;
    }

    (void) interrupt_detach(iid);
    return 0;
}

/* Read one "key=value" gauge from /sys/kstats (Q02). Returns -1 if absent. */
static long kstat_get(const char *key)
{
    char buf[8192];
    char *p;
    ssize_t n;
    size_t klen = strlen(key);
    int fd = open("/sys/kstats", O_RDONLY);

    if (fd == -1)
        return -1;
    n = read(fd, buf, sizeof buf - 1);
    (void) close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    for (p = buf; p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, klen) && p[klen] == '=')
            return strtol(p + klen + 1, NULL, 10);
    return -1;
}

struct ist_exit_ctx {
    int vector;
    volatile int id;   /* attach result + 1; 0 until the worker has attached */
    volatile int woke; /* 1 once the worker's wait returned */
};

/* Thread-bound IST that exits without detaching: thread death must release the
 * attachment (kernel/thread/lifecycle.c).
 */
static void *ist_exit_worker(void *arg)
{
    struct ist_exit_ctx *c = arg;
    uint64_t guard_ns;
    int id;

    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        c->id = -1;
        return NULL;
    }
    id = interrupt_attach_thread(c->vector, 0);
    c->id = id == -1 ? -1 : id + 1; /* +1: a valid id may be 0 */
    if (id == -1)
        return NULL;
    guard_ns = EXIT_GUARD_NS;
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_INTR, NULL, &guard_ns,
                           NULL);
    if (interrupt_wait(0, NULL) == 0)
        c->woke = 1;
    rtc_clear(&probe);
    return NULL; /* exits with the attachment still live */
}

static int test_ist_exit(int vector)
{
    /* interrupt_wait itself rejects a timeout pointer (ENOTSUP); bound the wait
     * with a one-shot timer_timeout on STATE_INTR instead.
     */
    uint64_t wait_ns = EXIT_WAIT_NS;
    long used_base = -1, used_end;

    for (int round = 0; round < EXIT_ROUNDS; round++) {
        struct ist_exit_ctx c = {.vector = vector};
        struct timespec nap = {.tv_sec = 0, .tv_nsec = 100000};
        pthread_t th;
        int spins = 0, old_id, iid, wakes;

        if (round == 1)
            used_base = kstat_get("soul.interrupt.used");
        if (pthread_create(&th, NULL, ist_exit_worker, &c) != 0) {
            fprintf(stderr, "irq-notify: FAIL ist-exit round %d: create\n",
                    round);
            return -1;
        }
        while (!c.id && spins++ < 20000)
            (void) nanosleep(&nap, NULL);
        if (c.id <= 0) {
            fprintf(stderr, "irq-notify: FAIL ist-exit round %d: attach\n",
                    round);
            return -1;
        }
        old_id = c.id - 1;
        rtc_arm(&probe, rtc_now(&probe) + ALARM_DELTA_NS, 1);
        (void) pthread_join(th, NULL);
        if (!c.woke) {
            fprintf(stderr, "irq-notify: FAIL ist-exit round %d: no wake\n",
                    round);
            return -1;
        }
        /* Thread death released the attachment, so the id is gone: the kernel
         * answers ESRCH (no such interrupt id), not EINVAL.
         */
        errno = 0;
        if (interrupt_detach(old_id) != -1 || errno != ESRCH) {
            fprintf(stderr,
                    "irq-notify: FAIL ist-exit round %d: stale detach of id %d "
                    "errno=%d (expected -1/ESRCH)\n",
                    round, old_id, errno);
            return -1;
        }

        rtc_reset(&probe);
        iid = interrupt_attach_thread(vector, 0);
        if (iid == -1) {
            fprintf(stderr,
                    "irq-notify: FAIL ist-exit round %d: re-attach: %s\n",
                    round, strerror(errno));
            return -1;
        }
        rtc_arm(&probe, rtc_now(&probe) + ALARM_DELTA_NS, 1);
        wait_ns = EXIT_GUARD_NS;
        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_INTR, NULL,
                               &wait_ns, NULL);
        wakes = interrupt_wait(0, NULL) == 0;
        wait_ns = EXIT_WAIT_NS;
        rtc_clear(&probe);
        (void) __interrupt_unmask(vector, iid);
        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_INTR, NULL,
                               &wait_ns, NULL);
        errno = 0;
        if (!wakes || interrupt_wait(0, NULL) != -1 || errno != ETIMEDOUT) {
            fprintf(stderr,
                    "irq-notify: FAIL ist-exit round %d: replacement wake=%d, "
                    "extra wait errno=%d (expected one wake then ETIMEDOUT)\n",
                    round, wakes, errno);
            return -1;
        }
        (void) interrupt_detach(iid);
        rtc_reset(&probe);
    }
    used_end = kstat_get("soul.interrupt.used");
    if (used_base < 0 || used_base != used_end) {
        fprintf(stderr,
                "irq-notify: FAIL ist-exit: soul.interrupt.used %ld -> %ld\n",
                used_base, used_end);
        return -1;
    }
    printf("irq-notify: PASS ist-exit rounds=%d soul.interrupt.used=%ld\n",
           EXIT_ROUNDS, used_end);
    return 0;
}

static int judge(const struct path_result *chan,
                 const struct path_result *ist)
{
    unsigned ok = chan->counts[RTC_OUTCOME_OK] + ist->counts[RTC_OUTCOME_OK];
    unsigned early =
        chan->counts[RTC_OUTCOME_EARLY] + ist->counts[RTC_OUTCOME_EARLY];
    unsigned tmo =
        chan->counts[RTC_OUTCOME_TIMEOUT] + ist->counts[RTC_OUTCOME_TIMEOUT];

    printf("irq-notify: outcomes ok=%u early=%u timeout=%u attempts=%u "
           "suppress_one=%d\n",
           ok, early, tmo, probe.attempts, suppress_one);
    if (ok + early + tmo != probe.attempts ||
        probe.attempts != 2 * LAT_ITERS) {
        printf("irq-notify: FAIL outcome accounting: %u+%u+%u != %u attempts\n",
               ok, early, tmo, probe.attempts);
        return -1;
    }
    if (!suppress_one) {
        if (early || tmo) {
            printf("irq-notify: FAIL early=%u timeout=%u (expected 0 and 0)\n",
                   early, tmo);
            return -1;
        }
        return 0;
    }
    if (early || tmo != 1 || suppress_timeouts != 1 ||
        suppress_window_ok != 1 || ok != probe.attempts - 1) {
        printf("irq-notify: FAIL suppress-one: ok=%u early=%u timeout=%u "
               "suppressed_timeouts=%u in_window=%u elapsed_ns=%llu "
               "(expected exactly one timeout, on the suppressed attempt, "
               "%llu-%llu ns after arm)\n",
               ok, early, tmo, suppress_timeouts, suppress_window_ok,
               (unsigned long long) suppress_elapsed_ns,
               (unsigned long long) RTC_PROBE_SUPPRESS_MIN_NS,
               (unsigned long long) RTC_PROBE_SUPPRESS_MAX_NS);
        return -1;
    }
    printf("irq-notify: suppress-one timeout=1 elapsed_ns=%llu window=[%llu,%llu]\n",
           (unsigned long long) suppress_elapsed_ns,
           (unsigned long long) RTC_PROBE_SUPPRESS_MIN_NS,
           (unsigned long long) RTC_PROBE_SUPPRESS_MAX_NS);
    return 0;
}

int main(int argc, char **argv)
{
    unsigned long phys = 0;
    int irq = -1, vector;
    uint64_t seed = 0, chan_p50, chan_max, ist_p50, ist_max;
    static struct path_result chan, ist;
    volatile unsigned char *rtc;
    struct sched_param sp = {.sched_priority = IRQ_PRIORITY};

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--suppress-one")) {
            suppress_one = 1;
        } else if (!strncmp(argv[i], "--seed=", 7)) {
            seed = strtoull(argv[i] + 7, NULL, 0);
        } else {
            fprintf(stderr, "irq-notify: usage: irq-notify [--suppress-one] "
                            "[--seed=N]\n");
            return 1;
        }
    }

    /* Run at a realtime priority so the wake-latency measurement is not
     * polluted by scheduling delay behind other threads (rt-latency and
     * timer-jitter do the same), and so an earlier stage's leftover threads
     * cannot starve this probe when it runs inside the rt-check sequence.
     */
    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1) {
        fprintf(stderr, "irq-notify: __sched_set: %s\n", strerror(errno));
        return 1;
    }
    if (thread_ctl(RTMUX_TCTL_IO, 0) == -1) {
        fprintf(stderr, "irq-notify: thread_ctl IO: %s\n", strerror(errno));
        return 1;
    }
    if (rtc_delegate(&phys, &irq) != EOK || phys == 0 || irq < 0) {
        fprintf(stderr, "irq-notify: no delegated goldfish-rtc MMIO\n");
        return 1;
    }
    vector = PLIC_VECTOR_BASE + irq;

    rtc = mmap(0, 0x1000, PROT_READ | PROT_WRITE | PROT_NOCACHE,
               MAP_PHYS | MAP_SHARED, NOFD, (off_t) phys);
    if (rtc == MAP_FAILED) {
        fprintf(stderr, "irq-notify: mmap RTC: %s\n", strerror(errno));
        return 1;
    }
    rtc_probe_init(&probe, rtc, seed, "irq-notify");
    if (suppress_one) {
        /* One of the 2 * LAT_ITERS latency attempts, chosen by the seed, so
         * either wait path can be the one that must hit its deadline.
         */
        suppress_at = (int) (rtc_probe_next(&probe) % (2 * LAT_ITERS));
        printf("irq-notify: suppress-one attempt=%d\n", suppress_at);
    }
    rtc_reset(&probe);

    if (test_channel_priority(vector, &chan) != 0)
        return 1;
    if (test_dedicated(vector, &ist) != 0)
        return 1;
    if (judge(&chan, &ist) != 0)
        return 1;
    /* The ist-exit rounds are an unrelated lifecycle check; keep the suppress
     * run to the one deadline it exists to prove.
     */
    if (!suppress_one && test_ist_exit(vector) != 0)
        return 1;

    rtc_probe_stats(chan.lat, chan.nok, &chan_p50, &chan_max);
    rtc_probe_stats(ist.lat, ist.nok, &ist_p50, &ist_max);
    printf("irq-notify: rtc phys=%#lx irq=%d vector=%d iters=%d seed=%llu "
           "chan_ok=%u chan_p50_ns=%llu chan_max_ns=%llu ist_ok=%u "
           "ist_p50_ns=%llu ist_max_ns=%llu\n",
           phys, irq, vector, LAT_ITERS, (unsigned long long) probe.seed,
           chan.nok, (unsigned long long) chan_p50,
           (unsigned long long) chan_max, ist.nok,
           (unsigned long long) ist_p50, (unsigned long long) ist_max);
    printf("irq-notify: PASS unified channel-routed IRQ works, priority order "
           "honored, both paths measured\n");
    return 0;
}
