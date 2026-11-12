/*
 * rtc-probe.h - shared goldfish-rtc alarm probe for the IRQ-latency fixtures.
 *
 * irq-notify and irq-latency both drive a real hardware IRQ from the QEMU virt
 * goldfish-rtc alarm.  This header holds the one copy of the RTC register
 * access and the attempt discipline, so the two fixtures cannot drift:
 *
 *   - the alarm delay of every attempt is drawn from a seeded generator,
 *     uniform in [1, 10] ms; the seed is printed (and can be replayed with
 *     --seed=N), so a fixed-phase artifact against the tick or the RTC cannot
 *     hide in a constant delay;
 *   - every wait carries a 1 s deadline (RTC_PROBE_DEADLINE_NS): a lost IRQ is
 *     a `timeout` outcome, never a hang.  The deadline is a one-shot
 *     __timer_timeout armed for the blocking state the caller waits in
 *     (RTMUX_TIMEOUT_RECEIVE for MsgReceive, RTMUX_TIMEOUT_INTR for
 *     interrupt_wait);
 *   - each attempt records the programmed alarm, the RTC read before and after
 *     arming, and the RTC read at wake, and yields exactly one outcome:
 *       ok       woke at or after the programmed alarm
 *       early    woke before the programmed alarm (a stale or spurious wake)
 *       timeout  the 1 s deadline expired with no wake
 *     An attempt is never retaken: a bad one is counted and reported.  A wait
 *     that fails for any other reason aborts the probe (rtc_probe_attempt
 *     returns -1) rather than inventing a fourth outcome;
 *   - the alarm and the asserted IRQ are cleared before each attempt, so one
 *     attempt cannot be satisfied by the leftovers of the previous one.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef RT_LATENCY_RTC_PROBE_H
#define RT_LATENCY_RTC_PROBE_H

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/rtmux-core.h>

#include "alarm-device.h"

#define RTC_PROBE_DELAY_MIN_NS 1000000ULL       /* 1 ms */
#define RTC_PROBE_DELAY_MAX_NS 10000000ULL      /* 10 ms */
#define RTC_PROBE_DEADLINE_NS 1000000000ULL     /* 1 s on every wait */
#define RTC_PROBE_SUPPRESS_MIN_NS 1000000000ULL /* timeout window, 1.0 s */
#define RTC_PROBE_SUPPRESS_MAX_NS 1100000000ULL /* ... to 1.1 s after arm */
#define RTC_PROBE_MAX_ATTEMPTS 256

enum rtc_outcome { RTC_OUTCOME_OK, RTC_OUTCOME_EARLY, RTC_OUTCOME_TIMEOUT };

struct rtc_attempt {
    unsigned index;       /* attempt number within the probe, from 0 */
    int suppressed;       /* alarm programmed but its IRQ gated off */
    uint64_t delay_ns;    /* seeded delay: alarm - pre_arm */
    uint64_t alarm_ns;    /* programmed alarm, RTC ns */
    uint64_t pre_arm_ns;  /* RTC read before arming */
    uint64_t post_arm_ns; /* RTC read after arming */
    uint64_t wake_ns;     /* RTC read once the wait returned */
    enum rtc_outcome outcome;
};

struct rtc_probe {
    volatile unsigned char *base; /* mapped RTC MMIO */
    uint64_t seed;                /* printed, replayable with --seed=N */
    uint64_t rng;
    unsigned attempts;
    unsigned counts[3]; /* indexed by enum rtc_outcome */
};

/* Wait hooks supplied by the fixture.  prepare unmasks the vector (called with
 * the RTC alarm and IRQ already cleared, before the alarm is armed); wait must
 * block until the IRQ wakes the fixture and return 0, ETIMEDOUT if the deadline
 * it armed through rtc_probe_deadline() expired, or another errno on failure.
 */
struct rtc_probe_ops {
    void (*prepare)(void *ctx);
    int (*wait)(void *ctx);
};

static inline uint64_t rtc_now(const struct rtc_probe *p)
{
    return alarm_now(p->base);
}
static inline void rtc_arm(const struct rtc_probe *p, uint64_t when, int enabled)
{
    alarm_arm(p->base, when, enabled);
}
static inline void rtc_clear(const struct rtc_probe *p)
{
    alarm_clear(p->base);
}
static inline void rtc_reset(const struct rtc_probe *p)
{
    alarm_reset(p->base);
}

static inline uint64_t rtc_probe_mono_ns(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* Arm the 1 s deadline for the next blocking call in `state_flag`
 * (RTMUX_TIMEOUT_RECEIVE / RTMUX_TIMEOUT_INTR).  One-shot: the kernel drops it
 * when that call returns, blocked or not.  Returns 0, or -1 with errno set.
 */
static inline int rtc_probe_deadline(int state_flag)
{
    uint64_t ns = RTC_PROBE_DEADLINE_NS;

    return __timer_timeout(CLOCK_MONOTONIC, state_flag, NULL, &ns, NULL);
}

/* Shared hooks for the dedicated-IST wait (interrupt_attach_thread +
 * interrupt_wait), used by both fixtures.  ctx is a struct rtc_irq_ctx.
 */
struct rtc_irq_ctx {
    int vector, id;
};

static inline void rtc_irq_prepare(void *ctx)
{
    struct rtc_irq_ctx *c = ctx;

    (void) __interrupt_unmask(c->vector, c->id);
}

static inline int rtc_ist_wait(void *ctx)
{
    (void) ctx;
    if (rtc_probe_deadline(RTMUX_TIMEOUT_INTR) == -1)
        return errno;
    if (interrupt_wait(0, NULL) == -1)
        return errno ? errno : EIO;
    return 0;
}

static inline uint64_t rtc_probe_next(struct rtc_probe *p)
{
    /* xorshift64*: never zero-state, period 2^64-1. */
    uint64_t x = p->rng;

    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    p->rng = x;
    return x * 0x2545f4914f6cdd1dULL;
}

/* Seed from the caller (--seed=N) or, when seed_in is 0, from the monotonic and
 * RTC clocks.  Prints the seed so any run can be replayed.
 */
static inline void rtc_probe_init(struct rtc_probe *p,
                                  volatile unsigned char *base,
                                  uint64_t seed_in,
                                  const char *tag)
{
    memset(p, 0, sizeof *p);
    p->base = base;
    p->seed = seed_in ? seed_in : (rtc_probe_mono_ns() ^ rtc_now(p) ^ 1ULL);
    p->rng = p->seed ? p->seed : 1ULL;
    printf("%s: seed=%llu delay_ns=[%llu,%llu] deadline_ns=%llu\n", tag,
           (unsigned long long) p->seed,
           (unsigned long long) RTC_PROBE_DELAY_MIN_NS,
           (unsigned long long) RTC_PROBE_DELAY_MAX_NS,
           (unsigned long long) RTC_PROBE_DEADLINE_NS);
}

/* Uniform draw in [1, 10] ms. */
static inline uint64_t rtc_probe_delay(struct rtc_probe *p)
{
    uint64_t span = RTC_PROBE_DELAY_MAX_NS - RTC_PROBE_DELAY_MIN_NS + 1;

    return RTC_PROBE_DELAY_MIN_NS + rtc_probe_next(p) % span;
}

static inline const char *rtc_outcome_name(enum rtc_outcome o)
{
    return o == RTC_OUTCOME_OK      ? "ok"
           : o == RTC_OUTCOME_EARLY ? "early"
                                    : "timeout";
}

/* Run one attempt and fill *a.  Returns 0 with exactly one outcome recorded, or
 * -1 (errno set, message printed) if the wait failed for a reason other than the
 * deadline, in which case nothing is counted and the caller must abort.
 * suppress != 0 programs the alarm with its IRQ gated off, so the attempt must
 * end in the deadline.
 */
static inline int rtc_probe_attempt(struct rtc_probe *p,
                                    const struct rtc_probe_ops *ops,
                                    void *ctx,
                                    int suppress,
                                    const char *tag,
                                    const char *path,
                                    struct rtc_attempt *a)
{
    int rc;

    memset(a, 0, sizeof *a);
    a->index = p->attempts;
    a->suppressed = suppress;

    /* Alarm and IRQ cleared first, then the vector unmasked, then armed. */
    rtc_reset(p);
    ops->prepare(ctx);

    a->delay_ns = rtc_probe_delay(p);
    a->pre_arm_ns = rtc_now(p);
    a->alarm_ns = a->pre_arm_ns + a->delay_ns;
    rtc_arm(p, a->alarm_ns, !suppress);
    a->post_arm_ns = rtc_now(p);

    rc = ops->wait(ctx);
    a->wake_ns = rtc_now(p);

    if (rc == 0)
        a->outcome =
            a->wake_ns >= a->alarm_ns ? RTC_OUTCOME_OK : RTC_OUTCOME_EARLY;
    else if (rc == ETIMEDOUT)
        a->outcome = RTC_OUTCOME_TIMEOUT;
    else {
        fprintf(stderr, "%s: FAIL %s attempt %u: wait: %s\n", tag, path,
                a->index, strerror(rc));
        rtc_reset(p);
        errno = rc;
        return -1;
    }
    /* Leave nothing armed or asserted for whatever runs next. */
    rtc_reset(p);

    p->attempts++;
    p->counts[a->outcome]++;
    printf("%s: attempt path=%s n=%u delay_ns=%llu alarm=%llu pre=%llu "
           "post=%llu wake=%llu outcome=%s%s lat_ns=%lld elapsed_ns=%llu\n",
           tag, path, a->index, (unsigned long long) a->delay_ns,
           (unsigned long long) a->alarm_ns,
           (unsigned long long) a->pre_arm_ns,
           (unsigned long long) a->post_arm_ns,
           (unsigned long long) a->wake_ns, rtc_outcome_name(a->outcome),
           suppress ? " suppressed" : "",
           (long long) (a->wake_ns - a->alarm_ns),
           (unsigned long long) (a->wake_ns - a->post_arm_ns));
    return 0;
}

/* Median and maximum of n recorded ok latencies (wake - alarm). */
static inline void rtc_probe_stats(const uint64_t *lat,
                                   unsigned n,
                                   uint64_t *p50,
                                   uint64_t *max)
{
    uint64_t s[RTC_PROBE_MAX_ATTEMPTS];
    unsigned i, j;

    *p50 = *max = 0;
    if (n == 0 || n > RTC_PROBE_MAX_ATTEMPTS)
        return;
    for (i = 0; i < n; i++) {
        uint64_t v = lat[i];

        for (j = i; j > 0 && s[j - 1] > v; j--)
            s[j] = s[j - 1];
        s[j] = v;
    }
    *p50 = s[n / 2];
    *max = s[n - 1];
}

#endif /* RT_LATENCY_RTC_PROBE_H */
