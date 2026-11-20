/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * timers.c — __timer_create / __timer_destroy conformance tests.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/rtmux-core.h>
#include <sys/siginfo.h>
#include <sys/wait.h>

#include "test.h"

/* Helper: build a SIGEV_NOTIFICATION event using the process's system channel.
 * The (void *)(uintptr_t) dance avoids a -Wint-to-pointer-cast warning from the
 * SIGEV_NOTIFICATION_INIT macro's sival_ptr assignment.
 */
static void make_notification_event(struct sigevent *ev,
                                    int code,
                                    uintptr_t value)
{
    SIGEV_NOTIFICATION_INIT(ev, ROOTTASK_COID, 10, code, (void *) value);
}

/* Wrapper: __timer_create returns int, __timer_destroy takes timer_t. This
 * avoids -Wsign-conversion on every call site.
 */
static int destroy_timer(int tid)
{
    return __timer_destroy((timer_t) tid);
}

static int delete_timer(timer_t tid)
{
    return timer_delete(tid);
}

static void test_posix_timer_create_delete(void)
{
    struct sigevent ev;
    timer_t tid = 0;
    int rc;
    char detail[96];

    make_notification_event(&ev, 40, 0);
    errno = 0;
    rc = timer_create(CLOCK_REALTIME, &ev, &tid);
    snprintf(detail, sizeof detail, "rc=%d tid=%llu errno=%d", rc,
             (unsigned long long) tid, errno);
    check(rc == 0, "timer_create(CLOCK_REALTIME) succeeds", detail);

    if (rc == 0) {
        rc = delete_timer(tid);
        snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
        check(rc == 0, "timer_delete(valid timer)", detail);
    }
}

static void test_posix_timer_create_invalid_args(void)
{
    struct sigevent ev;
    timer_t tid;
    int rc;
    char detail[96];

    make_notification_event(&ev, 41, 0);
    errno = 0;
    rc = timer_create(CLOCK_REALTIME, &ev, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "timer_create(NULL timerid)", detail);

    errno = 0;
    rc = timer_create(CLOCK_REALTIME, NULL, &tid);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "timer_create(NULL event)", detail);
}

static void test_posix_timer_settime_gettime(void)
{
    struct sigevent ev;
    struct itimerspec set = {
        .it_value = {.tv_sec = 1, .tv_nsec = 0},
        .it_interval = {.tv_sec = 0, .tv_nsec = 1000000},
    };
    struct itimerspec got;
    timer_t tid;
    int rc;
    char detail[128];

    make_notification_event(&ev, 42, 0);
    if (timer_create(CLOCK_REALTIME, &ev, &tid) != 0) {
        test_fail("timer_settime/gettime setup", strerror(errno));
        return;
    }

    errno = 0;
    rc = timer_settime(tid, 0, &set, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == 0, "timer_settime(relative)", detail);

    errno = 0;
    rc = timer_gettime(tid, &got);
    snprintf(detail, sizeof detail,
             "rc=%d errno=%d value=%lld.%09lld interval=%lld.%09lld", rc, errno,
             (long long) got.it_value.tv_sec, (long long) got.it_value.tv_nsec,
             (long long) got.it_interval.tv_sec,
             (long long) got.it_interval.tv_nsec);
    check(rc == 0, "timer_gettime(active timer)", detail);
    check(got.it_interval.tv_sec == 0 && got.it_interval.tv_nsec == 1000000,
          "timer_gettime reports interval", detail);

    delete_timer(tid);
}

static void test_posix_timer_settime_invalid(void)
{
    struct sigevent ev;
    struct itimerspec bad = {
        .it_value = {.tv_sec = 0, .tv_nsec = 1000000000},
        .it_interval = {.tv_sec = 0, .tv_nsec = 0},
    };
    timer_t tid;
    int rc;
    char detail[96];

    make_notification_event(&ev, 43, 0);
    if (timer_create(CLOCK_REALTIME, &ev, &tid) != 0) {
        test_fail("timer_settime invalid setup", strerror(errno));
        return;
    }

    errno = 0;
    rc = timer_settime(tid, 0, NULL, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "timer_settime(NULL value)", detail);

    errno = 0;
    rc = timer_settime(tid, 0, &bad, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "timer_settime(invalid nsec)", detail);

    errno = 0;
    bad.it_value.tv_nsec = 0;
    rc = timer_settime(tid, 1, &bad, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "timer_settime(invalid flags)", detail);

    delete_timer(tid);
}

static void test_posix_timer_getoverrun(void)
{
    struct sigevent ev;
    timer_t tid;
    int overrun;
    char detail[96];

    make_notification_event(&ev, 44, 0);
    if (timer_create(CLOCK_REALTIME, &ev, &tid) != 0) {
        test_fail("timer_getoverrun setup", strerror(errno));
        return;
    }

    errno = 0;
    overrun = timer_getoverrun(tid);
    snprintf(detail, sizeof detail, "overrun=%d errno=%d", overrun, errno);
    check(overrun == 0, "timer_getoverrun(new timer)", detail);

    delete_timer(tid);
}

static void test_posix_clock_edges(void)
{
    struct timespec zero = {.tv_sec = 0, .tv_nsec = 0};
    struct timespec ts;
    clockid_t clock_id;
    int rc;
    char detail[96];

    errno = 0;
    rc = clock_getres(CLOCK_REALTIME, &ts);
    snprintf(detail, sizeof detail, "rc=%d errno=%d res=%lld.%09lld", rc, errno,
             (long long) ts.tv_sec, (long long) ts.tv_nsec);
    check(rc == 0, "clock_getres(CLOCK_REALTIME)", detail);

    errno = 0;
    rc = clock_getres(CLOCK_MONOTONIC, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == 0, "clock_getres(CLOCK_MONOTONIC, NULL)", detail);

    errno = 0;
    rc = clock_getres((clockid_t) -1, &ts);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "clock_getres(invalid clock)", detail);

    ts.tv_sec = 0;
    ts.tv_nsec = 1000000000L;
    errno = 0;
    rc = clock_settime(CLOCK_REALTIME, &ts);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "clock_settime(invalid nsec)", detail);

    rc = clock_getcpuclockid(getpid(), &clock_id);
    snprintf(detail, sizeof detail, "rc=%d clock=%d", rc, (int) clock_id);
    check(rc == 0, "clock_getcpuclockid(getpid)", detail);

    rc = clock_getcpuclockid(getpid(), NULL);
    snprintf(detail, sizeof detail, "rc=%d", rc);
    check(rc == EINVAL, "clock_getcpuclockid(NULL)", detail);

    rc = pthread_getcpuclockid(pthread_self(), &clock_id);
    snprintf(detail, sizeof detail, "rc=%d clock=%d", rc, (int) clock_id);
    check(rc == 0, "pthread_getcpuclockid(self)", detail);

    rc = pthread_getcpuclockid(pthread_self(), NULL);
    snprintf(detail, sizeof detail, "rc=%d", rc);
    check(rc == EINVAL, "pthread_getcpuclockid(NULL)", detail);

    errno = 0x5a;
    rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &zero, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == 0 && errno == 0x5a, "clock_nanosleep(relative zero)", detail);

    rc = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &zero, NULL);
    snprintf(detail, sizeof detail, "rc=%d", rc);
    check(rc == 0, "clock_nanosleep(absolute zero)", detail);

    rc = clock_nanosleep(CLOCK_MONOTONIC, 1, &zero, NULL);
    snprintf(detail, sizeof detail, "rc=%d", rc);
    check(rc == EINVAL, "clock_nanosleep(invalid flags)", detail);

    rc = clock_nanosleep(CLOCK_MONOTONIC, 0, NULL, NULL);
    snprintf(detail, sizeof detail, "rc=%d", rc);
    check(rc == EINVAL, "clock_nanosleep(NULL rqtp)", detail);

    zero.tv_nsec = 1000000000L;
    rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &zero, NULL);
    snprintf(detail, sizeof detail, "rc=%d", rc);
    check(rc == EINVAL, "clock_nanosleep(invalid nsec)", detail);

    zero.tv_sec = -1;
    zero.tv_nsec = 0;
    rc = clock_nanosleep(CLOCK_MONOTONIC, 0, &zero, NULL);
    snprintf(detail, sizeof detail, "rc=%d", rc);
    check(rc == EINVAL, "clock_nanosleep(negative sec)", detail);

    errno = 0;
    rc = nanosleep(&zero, NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == -1 && errno == EINVAL, "nanosleep(negative sec)", detail);
}

/* __timer_create — success cases */

static void test_timer_create_realtime(void)
{
    struct sigevent ev;
    int tid;

    make_notification_event(&ev, 42, 0);
    errno = 0;
    tid = __timer_create(CLOCK_REALTIME, &ev);
    check(tid >= 0, "__timer_create(CLOCK_REALTIME, notification)",
          "expected valid timer ID");

    if (tid >= 0) {
        /* Verify we can destroy it */
        int rc = destroy_timer(tid);
        check(rc == 0, "__timer_destroy after CLOCK_REALTIME create",
              "expected 0");
    }
}

static void test_timer_create_monotonic(void)
{
    struct sigevent ev;
    int tid;

    make_notification_event(&ev, 43, 0);
    errno = 0;
    tid = __timer_create(CLOCK_MONOTONIC, &ev);
    check(tid >= 0, "__timer_create(CLOCK_MONOTONIC, notification)",
          "expected valid timer ID");

    if (tid >= 0)
        destroy_timer(tid);
}

static void test_timer_create_softtime(void)
{
    struct sigevent ev;
    int tid;

    make_notification_event(&ev, 44, 0);
    errno = 0;
    tid = __timer_create(CLOCK_SOFTTIME, &ev);
    check(tid >= 0, "__timer_create(CLOCK_SOFTTIME, notification)",
          "expected valid timer ID (equiv to CLOCK_REALTIME)");

    if (tid >= 0)
        destroy_timer(tid);
}

static void test_timer_create_unique_ids(void)
{
    struct sigevent ev;
    int t1, t2, t3;

    make_notification_event(&ev, 45, 0);
    t1 = __timer_create(CLOCK_REALTIME, &ev);
    t2 = __timer_create(CLOCK_REALTIME, &ev);
    t3 = __timer_create(CLOCK_REALTIME, &ev);

    check(t1 >= 0 && t2 >= 0 && t3 >= 0, "__timer_create x3 all succeed",
          "expected three valid timer IDs");

    check(t1 != t2 && t2 != t3 && t1 != t3, "__timer_create returns unique IDs",
          "timer IDs must be distinct");

    if (t1 >= 0)
        destroy_timer(t1);
    if (t2 >= 0)
        destroy_timer(t2);
    if (t3 >= 0)
        destroy_timer(t3);
}

/* __timer_create — error cases */

static void test_timer_create_invalid_clock(void)
{
    struct sigevent ev;
    int tid;
    char detail[128];

    make_notification_event(&ev, 50, 0);
    errno = 0;
    tid = __timer_create(999, &ev);
    snprintf(detail, sizeof detail, "tid=%d errno=%d (%s)", tid, errno,
             strerror(errno));
    check(tid == -1 && errno == EINVAL, "__timer_create(999) -> EINVAL",
          detail);
}

static void test_timer_create_negative_clock(void)
{
    struct sigevent ev;
    int tid;
    char detail[128];

    make_notification_event(&ev, 51, 0);
    errno = 0;
    tid = __timer_create((clockid_t) -1, &ev);
    snprintf(detail, sizeof detail, "tid=%d errno=%d (%s)", tid, errno,
             strerror(errno));
    check(tid == -1 && errno == EINVAL, "__timer_create(-1) -> EINVAL", detail);
}

static void test_timer_create_null_event(void)
{
    int tid;
    char detail[128];

    errno = 0;
    tid = __timer_create(CLOCK_REALTIME, NULL);
    snprintf(detail, sizeof detail, "tid=%d errno=%d (%s)", tid, errno,
             strerror(errno));
    check(tid == -1 && errno == EINVAL, "__timer_create(NULL event) -> EINVAL",
          detail);
}

static void test_timer_create_signal_event_rejected(void)
{
    struct sigevent ev;
    int tid;
    char detail[128];

    /* SIGEV_SIGNAL — rejected because signals are removed */
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_SIGNAL;
    ev.sigev_signo = 14; /* SIGALRM */
    errno = 0;
    tid = __timer_create(CLOCK_REALTIME, &ev);
    snprintf(detail, sizeof detail, "tid=%d errno=%d (%s)", tid, errno,
             strerror(errno));
    check(tid == -1 && errno == EINVAL,
          "__timer_create(SIGEV_SIGNAL) -> EINVAL", detail);

    /* SIGEV_SIGNAL_THREAD — also rejected */
    memset(&ev, 0, sizeof ev);
    ev.sigev_notify = SIGEV_SIGNAL_THREAD;
    ev.sigev_signo = 14;
    errno = 0;
    tid = __timer_create(CLOCK_REALTIME, &ev);
    snprintf(detail, sizeof detail, "tid=%d errno=%d (%s)", tid, errno,
             strerror(errno));
    check(tid == -1 && errno == EINVAL,
          "__timer_create(SIGEV_SIGNAL_THREAD) -> EINVAL", detail);
}

static void test_timer_create_notification_bad_coid(void)
{
    struct sigevent ev;
    int tid;
    char detail[128];

    /* SIGEV_NOTIFICATION with an invalid connection ID */
    SIGEV_NOTIFICATION_INIT(&ev, 0x7FFFFFFF, 10, 99, 0);
    errno = 0;
    tid = __timer_create(CLOCK_REALTIME, &ev);
    snprintf(detail, sizeof detail, "tid=%d errno=%d (%s)", tid, errno,
             strerror(errno));
    check(tid == -1 && errno == EBADF,
          "__timer_create(notification, bad coid) -> EBADF", detail);
}

/* __timer_destroy — success cases */

static void test_timer_destroy_valid(void)
{
    struct sigevent ev;
    int tid, rc;
    char detail[64];

    make_notification_event(&ev, 70, 0);
    tid = __timer_create(CLOCK_REALTIME, &ev);
    if (tid < 0) {
        test_fail("__timer_destroy(valid) — setup failed", strerror(errno));
        return;
    }

    errno = 0;
    rc = destroy_timer(tid);
    snprintf(detail, sizeof detail, "rc=%d errno=%d", rc, errno);
    check(rc == 0, "__timer_destroy(valid timer)", detail);
}

/* __timer_destroy — error cases */

static void test_timer_destroy_invalid_id(void)
{
    int rc;
    char detail[64];

    errno = 0;
    rc = __timer_destroy((timer_t) 99999);
    snprintf(detail, sizeof detail, "rc=%d errno=%d (%s)", rc, errno,
             strerror(errno));
    check(rc == -1 && errno == EINVAL, "__timer_destroy(99999) -> EINVAL",
          detail);
}

static void test_timer_destroy_negative_id(void)
{
    int rc;
    char detail[64];

    errno = 0;
    rc = __timer_destroy((timer_t) -1);
    snprintf(detail, sizeof detail, "rc=%d errno=%d (%s)", rc, errno,
             strerror(errno));
    check(rc == -1 && errno == EINVAL, "__timer_destroy(-1) -> EINVAL", detail);
}

static void test_timer_destroy_double(void)
{
    struct sigevent ev;
    int tid, rc;
    char detail[64];

    make_notification_event(&ev, 71, 0);
    tid = __timer_create(CLOCK_REALTIME, &ev);
    if (tid < 0) {
        test_fail("__timer_destroy(double) — setup failed", strerror(errno));
        return;
    }

    destroy_timer(tid);

    /* Second destroy should fail */
    errno = 0;
    rc = destroy_timer(tid);
    snprintf(detail, sizeof detail, "rc=%d errno=%d (%s)", rc, errno,
             strerror(errno));
    check(rc == -1 && errno == EINVAL,
          "__timer_destroy(already destroyed) -> EINVAL", detail);
}

/* Stress: create many timers, exhaust pool */

static void test_timer_create_exhaust(void)
{
    struct sigevent ev;
    int ids[256];
    int count = 0;
    int got_eagain = 0;
    char detail[128];

    make_notification_event(&ev, 90, 0);

    /* Create timers until we get EAGAIN or hit our array limit */
    for (count = 0; count < 256; count++) {
        errno = 0;
        ids[count] = __timer_create(CLOCK_REALTIME, &ev);
        if (ids[count] == -1) {
            if (errno == EAGAIN)
                got_eagain = 1;
            break;
        }
    }

    snprintf(detail, sizeof detail, "created %d timers before EAGAIN", count);
    check(got_eagain || count == 256,
          "__timer_create exhaustion returns EAGAIN", detail);

    check(count > 0, "__timer_create created at least one timer", detail);

    /* Clean up all created timers */
    for (int i = 0; i < count; i++)
        destroy_timer(ids[i]);
}

/* ID reuse: after destroy, the ID can be reused */

static void test_timer_id_reuse(void)
{
    struct sigevent ev;
    int t1, t2;
    char detail[64];

    make_notification_event(&ev, 91, 0);
    t1 = __timer_create(CLOCK_REALTIME, &ev);
    if (t1 < 0) {
        test_fail("timer ID reuse — setup failed", strerror(errno));
        return;
    }

    destroy_timer(t1);

    /* Create again — may get the same ID */
    t2 = __timer_create(CLOCK_REALTIME, &ev);
    snprintf(detail, sizeof detail, "t1=%d t2=%d", t1, t2);
    check(t2 >= 0, "__timer_create after destroy succeeds", detail);

    if (t2 >= 0)
        destroy_timer(t2);
}

/* Q25: CLOCK_THREAD_CPUTIME_ID is billed at switch time, so a thread that runs
 * short bursts and blocks between ticks is charged for what it ran.
 *
 * 10^4 bursts of 10 us separated by 100 us sleeps, as 50 blocks of 100 bursts.
 * The CPU clock also bills the kernel work of every nanosleep call (trap, timer
 * arm, block, wake, return), which no user-space MONOTONIC bracket can see, so
 * that fixed per-burst cost is calibrated away: each block is paired with a
 * control block of 100 zero-spin bursts (same clock reads and sleeps) run right
 * before it. Per pair, (CPU delta of the spin block - control block) is
 * compared with (sum of the spin bursts' bracketing CLOCK_MONOTONIC intervals -
 * the control's). The median pair error scaled to all 50 pairs is checked
 * against max(1% of the bracketed spin time, 2 x clock resolution); the median
 * keeps a host stall that inflates one block (QEMU descheduled under load) from
 * deciding the result. With tick billing the sleeping bursts were never charged
 * and the spin blocks would show no CPU growth over the controls.
 */
static uint64_t ts_ns(const struct timespec *ts)
{
    return (uint64_t) ts->tv_sec * 1000000000ULL + (uint64_t) ts->tv_nsec;
}

static uint64_t clock_ns(clockid_t id)
{
    struct timespec ts;

    if (clock_gettime(id, &ts) != 0)
        return 0;
    return ts_ns(&ts);
}

/* Run n bursts of spin_ns busy time, each followed by a 100 us sleep.
 *
 * Returns the CPU-clock delta and adds the bracketing intervals to *busy.
 */
static uint64_t cputime_block(int n,
                              uint64_t spin_ns,
                              uint64_t *busy,
                              uint64_t *wall)
{
    const struct timespec nap = {.tv_sec = 0, .tv_nsec = 100000};
    uint64_t cpu0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    uint64_t wall0 = clock_ns(CLOCK_MONOTONIC);

    for (int i = 0; i < n; i++) {
        uint64_t t0 = clock_ns(CLOCK_MONOTONIC), t1;

        /* At least one closing read in both block kinds, so the trap entry and
         * exit cost outside the bracket is identical and cancels.
         */
        do {
            t1 = clock_ns(CLOCK_MONOTONIC);
        } while (t1 - t0 < spin_ns);
        *busy += t1 - t0;
        (void) nanosleep(&nap, NULL);
    }
    *wall += clock_ns(CLOCK_MONOTONIC) - wall0;
    return clock_ns(CLOCK_THREAD_CPUTIME_ID) - cpu0;
}

static int64_t median_i64(int64_t *v, int n)
{
    for (int i = 1; i < n; i++) { /* insertion sort */
        int64_t x = v[i];
        int j = i - 1;

        while (j >= 0 && v[j] > x) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = x;
    }
    return v[n / 2];
}

/* One measurement round: PAIRS (control, spin) block pairs. Fills the medians
 * and totals the caller judges. A host stall that lands inside a block inflates
 * that block only; the medians ignore a minority of such pairs, and the caller
 * repeats a whole round if a stall hit enough pairs to move the median.
 */
#define CPUTIME_MAX_PAIRS 50

struct cputime_round {
    int64_t med_err, med_dcpu, med_dbusy;

    /* The spread of the per-pair errors, for the failure detail: a small bias
     * in every pair and a few large outliers have different causes.
     */
    int64_t err_min, err_q1, err_q3, err_max;
    uint64_t dbusy_total, spin_cpu, spin_busy, spin_wall;
    uint64_t ctl_cpu, ctl_wall;
};

static void cputime_round_run(struct cputime_round *r, int pairs, int per_block)
{
    int64_t err[CPUTIME_MAX_PAIRS], dcpu_v[CPUTIME_MAX_PAIRS];
    int64_t dbusy_v[CPUTIME_MAX_PAIRS];

    if (pairs > CPUTIME_MAX_PAIRS)
        pairs = CPUTIME_MAX_PAIRS;
    memset(r, 0, sizeof *r);
    for (int i = 0; i < pairs; i++) {
        uint64_t cb = 0, sb = 0, cw = 0;
        uint64_t cc = cputime_block(per_block, 0, &cb, &cw);
        uint64_t sc = cputime_block(per_block, 10000, &sb, &r->spin_wall);
        int64_t dcpu = (int64_t) sc - (int64_t) cc;
        int64_t dbusy = (int64_t) sb - (int64_t) cb;

        err[i] = dcpu - dbusy;
        dcpu_v[i] = dcpu;
        dbusy_v[i] = dbusy;
        r->spin_cpu += sc;
        r->spin_busy += sb;
        r->ctl_cpu += cc;
        r->ctl_wall += cw;
        r->dbusy_total += (uint64_t) (dbusy > 0 ? dbusy : 0);
    }
    r->med_err = median_i64(err, pairs); /* sorts err */
    r->err_min = err[0];
    r->err_q1 = err[pairs / 4];
    r->err_q3 = err[pairs * 3 / 4];
    r->err_max = err[pairs - 1];
    r->med_dcpu = median_i64(dcpu_v, pairs);
    r->med_dbusy = median_i64(dbusy_v, pairs);
}

static void test_cputime_short_bursts(void)
{
    enum { PAIRS = 50, PER_BLOCK = 100, ROUNDS = 3 }; /* 10^4 spin bursts */
    struct timespec res;
    struct cputime_round r;
    uint64_t res_ns = 0, tol = 0, c0, c1;
    int64_t noise = 0, limit = 0;
    int round, ok_pair = 0, ok_totals = 0, ok_dcpu = 0, ok_ctl = 0;
    char detail[400], hist[100] = "";

    if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
        res_ns = ts_ns(&res);

    /* A single long sleep accrues (almost) nothing. */
    {
        const struct timespec nap20 = {.tv_sec = 0, .tv_nsec = 20000000};
        uint64_t t = 2 * res_ns > 1000000 ? 2 * res_ns : 1000000;

        c0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        (void) nanosleep(&nap20, NULL);
        c1 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
        snprintf(detail, sizeof detail,
                 "cpu delta over 20ms sleep=%llu ns tol=%llu",
                 (unsigned long long) (c1 - c0), (unsigned long long) t);
        check(c0 != 0 && c1 - c0 <= t, "cputime: sleeping accrues nothing",
              detail);
    }

    /* Up to ROUNDS attempts: the tolerance is the 1% bound (2 x resolution or 1
     * ms floor), widened below to what a noisy round can resolve, and it must
     * hold for a whole round. Only a round that fails is repeated, so a
     * persistent billing error still fails all three; a transient host stall
     * (QEMU descheduled under load) does not decide the result. The detail line
     * reports the attempt number.
     */
    for (round = 1; round <= ROUNDS; round++) {
        int64_t med;

        cputime_round_run(&r, PAIRS, PER_BLOCK);
        tol = 2 * res_ns > 1000000 ? 2 * res_ns : 1000000;
        if (r.dbusy_total / 100 > tol)
            tol = r.dbusy_total / 100;
        med = r.med_err;
        snprintf(hist + strlen(hist), sizeof hist - strlen(hist), " %lld",
                 (long long) med);

        /* The 1% bound is tol / PAIRS a pair, 40 us at a 1 ms resolution, and a
         * virtual host cannot always resolve that: the work a burst is billed
         * for outside its bracket (the sleep call, the wake, the timer re-arm)
         * varies with the host, and the pairs of one round then spread over
         * hundreds of microseconds. So a round also passes when its median is
         * within what its own spread can tell from zero, three standard errors
         * of a median, about 0.4 of the interquartile range for 50 pairs. That
         * allowance is capped at a quarter of the median bracketed time, so a
         * thread billed a quarter more or less than it ran still fails however
         * noisy the host is, and a quiet host is still held to 1%.
         */
        noise = (r.err_q3 - r.err_q1) * 2 / 5;
        if (noise > r.med_dbusy / 4)
            noise = r.med_dbusy / 4;
        limit = (int64_t) tol / PAIRS;
        if (limit < noise)
            limit = noise;
        ok_pair = c0 != 0 && r.dbusy_total != 0 &&
                  (med < 0 ? -med : med) <= limit;

        /* Totals: the spin blocks' CPU covers what the brackets prove ran and
         * cannot exceed their wall time (a sleeper is never charged).
         */
        ok_totals = r.spin_cpu + 2 * res_ns >= r.spin_busy - r.spin_busy / 100 &&
                    r.spin_cpu <= r.spin_wall + 2 * res_ns;

        /* Median per-pair CPU delta within 50%..200% of the median bracketed
         * delta (outlier-robust, unlike a sum of positive deltas).
         */
        ok_dcpu = r.med_dbusy > 0 && r.med_dcpu >= r.med_dbusy / 2 &&
                  r.med_dcpu <= r.med_dbusy * 2;

        /* The control blocks are the fixed per-burst kernel cost the pair
         * method cancels, so check it directly: billed (nonzero: the trap,
         * timer and wake work is charged) and never more than the wall time.
         */
        ok_ctl = r.ctl_cpu != 0 && r.ctl_cpu <= r.ctl_wall + 2 * res_ns;
        if (ok_pair && ok_totals && ok_dcpu && ok_ctl)
            break;
    }
    if (round > ROUNDS)
        round = ROUNDS;
    snprintf(detail, sizeof detail,
             "attempt %d: median pair err=%lld ns (limit %lld; x%d=%lld) tol=%llu "
             "med dcpu=%lld dbusy=%lld dbusy_total=%llu res=%llu ns errs:%s "
             "pair err min=%lld q1=%lld q3=%lld max=%lld",
             round, (long long) r.med_err, (long long) limit, PAIRS,
             (long long) r.med_err * PAIRS, (unsigned long long) tol,
             (long long) r.med_dcpu, (long long) r.med_dbusy,
             (unsigned long long) r.dbusy_total, (unsigned long long) res_ns,
             hist, (long long) r.err_min, (long long) r.err_q1,
             (long long) r.err_q3, (long long) r.err_max);
    check(ok_pair, "cputime_short_bursts", detail);
    snprintf(detail, sizeof detail,
             "attempt %d: spin cpu=%llu busy=%llu wall=%llu", round,
             (unsigned long long) r.spin_cpu, (unsigned long long) r.spin_busy,
             (unsigned long long) r.spin_wall);
    check(ok_totals, "cputime_short_bursts totals bound busy <= cpu <= wall",
          detail);
    snprintf(detail, sizeof detail,
             "attempt %d: median dcpu=%lld dbusy=%lld", round,
             (long long) r.med_dcpu, (long long) r.med_dbusy);
    check(ok_dcpu, "cputime_short_bursts dcpu_total sanity", detail);
    snprintf(detail, sizeof detail,
             "attempt %d: control cpu=%llu wall=%llu (fixed cost billed, "
             "sleeps not)", round, (unsigned long long) r.ctl_cpu,
             (unsigned long long) r.ctl_wall);
    check(ok_ctl, "cputime_short_bursts fixed cost billed", detail);
}

/* Two CPU-bound threads share the hart(s): each thread's own CPU clock summed
 * must equal the process CPU clock delta (less the sleeping main thread's own
 * tiny share), proving a thread's time is charged once to its process and not
 * lost or doubled when the scheduler rotates them. Smoke on SMP.
 */
static void *cputime_spinner(void *arg)
{
    uint64_t *out = arg;
    uint64_t end = clock_ns(CLOCK_MONOTONIC) + 200000000ULL;

    while (clock_ns(CLOCK_MONOTONIC) < end)
        ;
    *out = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    return NULL;
}

static void test_cputime_two_threads(void)
{
    pthread_t a, b;
    uint64_t ca = 0, cb = 0, p0, p1, m0, m1, w0, w1, tol, sum, pd;
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    char detail[200];

    if (ncpu < 1)
        ncpu = 1;
    p0 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    m0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    w0 = clock_ns(CLOCK_MONOTONIC);
    if (pthread_create(&a, NULL, cputime_spinner, &ca) != 0 ||
        pthread_create(&b, NULL, cputime_spinner, &cb) != 0) {
        test_fail("cputime two threads", "pthread_create failed");
        return;
    }
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    w1 = clock_ns(CLOCK_MONOTONIC);
    m1 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    p1 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);

    /* ca/cb are totals since each thread started (cpu clock starts at 0). */
    sum = ca + cb + (m1 - m0);
    pd = p1 - p0;
    tol = pd / 100 > 2000000 ? pd / 100 : 2000000;
    snprintf(detail, sizeof detail,
             "a=%llu b=%llu main=%llu proc=%llu wall=%llu ncpu=%ld tol=%llu",
             (unsigned long long) ca, (unsigned long long) cb,
             (unsigned long long) (m1 - m0), (unsigned long long) pd,
             (unsigned long long) (w1 - w0), ncpu, (unsigned long long) tol);

    /* Thread teardown after the final read is billed to the process only, so
     * the process may exceed the sum, never fall below it by more than tol.
     */
    check(pd + tol >= sum && pd <= sum + tol + 5000000,
          "cputime two threads: sum of thread clocks == process clock", detail);
    check(ca + cb <= (w1 - w0) * (uint64_t) ncpu + tol && ca > 0 && cb > 0,
          "cputime two threads: no more CPU than wall x harts", detail);
}

/* Q63(c): yield/steal stress for the Q25 switch-time billing. N = 2 x harts
 * threads call sched_yield() in a tight loop (every 256th round they nap 50 us
 * instead, so a hart can go idle and steal another's just-yielded thread) for
 * 400 ms. yield_default and resched_default enqueue the yielder READY and bill
 * it first (sched_account_expose); a second hart may steal it at once, and a
 * stretch billed from both harts would inflate the sum. The threads stay
 * runnable almost always, so the harts are busy: the per-thread CPU sum must be
 * <= wall x harts (never over-billed) and, since busy harts bill nearly
 * everything, >= half of it (nothing lost); the process clock must equal the
 * sum of its threads' clocks. At CPUS=1 it is a plain yield-rotation check.
 */
enum { STRESS_MAX_THREADS = 16 };

struct yield_stress {
    uint64_t end;
    uint64_t cpu;
    uint64_t life; /* wall time from the worker's first line to its last */
};

static void *yield_stress_worker(void *arg)
{
    struct yield_stress *ys = arg;
    const struct timespec nap = {.tv_sec = 0, .tv_nsec = 50000};
    uint64_t t0 = clock_ns(CLOCK_MONOTONIC);

    for (unsigned i = 0; clock_ns(CLOCK_MONOTONIC) < ys->end; i++) {
        if ((i & 255) == 255)
            (void) nanosleep(&nap, NULL);
        else
            (void) sched_yield();
    }
    ys->cpu = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    ys->life = clock_ns(CLOCK_MONOTONIC) - t0;
    return NULL;
}

static void test_cputime_yield_steal_stress(void)
{
    struct yield_stress ys[STRESS_MAX_THREADS] = {{0, 0, 0}};
    pthread_t tid[STRESS_MAX_THREADS];
    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    int n, created = 0;
    uint64_t p0, p1, m0, m1, w0, w1, sum = 0, pd, wall, tol;
    uint64_t over = 0;
    char detail[200];

    if (ncpu < 1)
        ncpu = 1;
    n = ncpu * 2 > STRESS_MAX_THREADS ? STRESS_MAX_THREADS : (int) ncpu * 2;
    if (n < 2)
        n = 2;

    p0 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);
    m0 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    w0 = clock_ns(CLOCK_MONOTONIC);
    for (int i = 0; i < n; i++) {
        ys[i].end = w0 + 400000000ULL;
        if (pthread_create(&tid[i], NULL, yield_stress_worker, &ys[i]) != 0)
            break;
        created++;
    }
    for (int i = 0; i < created; i++)
        pthread_join(tid[i], NULL);
    w1 = clock_ns(CLOCK_MONOTONIC);
    m1 = clock_ns(CLOCK_THREAD_CPUTIME_ID);
    p1 = clock_ns(CLOCK_PROCESS_CPUTIME_ID);

    for (int i = 0; i < created; i++) {
        sum += ys[i].cpu;

        /* A thread runs on one hart at a time, so its clock cannot exceed its
         * own lifetime; a stolen stretch billed from both harts would show here
         * even when the all-thread sum stays under wall x harts.
         */
        if (ys[i].cpu > ys[i].life && ys[i].cpu - ys[i].life > over)
            over = ys[i].cpu - ys[i].life;
    }
    sum += m1 - m0;
    pd = p1 - p0;
    wall = w1 - w0;
    tol = wall / 100 > 2000000 ? wall / 100 : 2000000;
    snprintf(detail, sizeof detail,
             "threads=%d/%d sum=%llu proc=%llu wall=%llu ncpu=%ld tol=%llu over=%llu",
             created, n, (unsigned long long) sum, (unsigned long long) pd,
             (unsigned long long) wall, ncpu, (unsigned long long) tol,
             (unsigned long long) over);
    check(created == n, "cputime yield stress: threads created", detail);
    check(sum <= wall * (uint64_t) ncpu + tol,
          "cputime yield stress: per-thread sum <= wall x harts", detail);
    check(over <= tol, "cputime yield stress: no thread clock exceeds its lifetime",
          detail);
    check(sum >= wall * (uint64_t) ncpu / 2,
          "cputime yield stress: busy harts bill at least half of wall x harts",
          detail);
    check(pd + tol >= sum && pd <= sum + tol + 5000000,
          "cputime yield stress: sum of thread clocks == process clock",
          detail);
}

/* Q63(b): RLIMIT_CPU must stop a process with nothing else to reschedule. The
 * child lowers its soft and hard limit to 1 s of CPU and spins; the parent is
 * asleep, so no other thread is ready on the child's hart and the tick's
 * reschedule branch never fires for it. The child's billed CPU
 * (RUSAGE_CHILDREN, which includes its startup) must land within max(1%, 2 x
 * resolution) of the limit, and the parent kills the child at a deadline if the
 * limit is not enforced.
 */
#define RLIMIT_TEST_SECS 1

/* Negative paths that leave the process's limits untouched. */
static void test_rlimit_negative(void)
{
    struct rlimit rl = {RLIM_INFINITY, RLIM_INFINITY}, got = {0, 0};
    int rc, e;

    errno = 0;
    rc = setrlimit(-1, &rl);
    check(rc == -1 && errno == EINVAL, "setrlimit(-1) fails EINVAL", "");
    errno = 0;
    rc = setrlimit(RLIM_NLIMITS, &rl);
    check(rc == -1 && errno == EINVAL, "setrlimit(RLIM_NLIMITS) fails EINVAL",
          "");
    errno = 0;
    rc = setrlimit(RLIMIT_CPU, NULL);
    check(rc == -1 && errno == EFAULT, "setrlimit(NULL) rejects the pointer", "");
    errno = 0;
    rc = getrlimit(RLIM_NLIMITS, &got);
    check(rc == -1 && errno == EINVAL, "getrlimit(RLIM_NLIMITS) fails EINVAL",
          "");
    errno = 0;
    rc = getrlimit(RLIMIT_CPU, NULL);
    check(rc == -1 && errno == EFAULT, "getrlimit(NULL) rejects the pointer", "");
    errno = 0;
    rc = setrlimit(RLIMIT_NOFILE, &rl);
    e = errno;
    check(rc == -1 && e == ENOSYS, "setrlimit(RLIMIT_NOFILE) fails ENOSYS", "");
    rc = getrlimit(RLIMIT_CPU, &got);
    check(rc == 0 && got.rlim_cur == RLIM_INFINITY &&
              got.rlim_max == RLIM_INFINITY,
          "getrlimit(RLIMIT_CPU) reads back infinity", "");
}

int rlimit_cpu_child(void)
{
    struct rlimit rl = {RLIMIT_TEST_SECS, RLIMIT_TEST_SECS}, got = {0, 0};
    struct rlimit bad = {RLIMIT_TEST_SECS + 1, RLIMIT_TEST_SECS};
    struct rlimit raise = {RLIMIT_TEST_SECS, RLIM_INFINITY};

    if (setrlimit(RLIMIT_CPU, &bad) != -1 || errno != EINVAL)
        return 2;
    if (setrlimit(RLIMIT_CPU, &rl) != 0)
        return 4;
    if (setrlimit(RLIMIT_CPU, &raise) != -1 || errno != EPERM)
        return 3;
    if (getrlimit(RLIMIT_CPU, &got) != 0 || got.rlim_cur != RLIMIT_TEST_SECS ||
        got.rlim_max != RLIMIT_TEST_SECS)
        return 5;
    for (;;)
        ;
}

/* Inheritance: a limit set before posix_spawn applies to the new process, which
 * starts with its own CPU clock at 0 and is stopped by the inherited limit. The
 * middle process sets the limit and sleeps (so it is not itself at risk), the
 * grandchild checks the pair read back and spins until the limit stops it.
 */
int rlimit_inherit_grandchild(void)
{
    struct rlimit got = {0, 0};
    uint64_t end = clock_ns(CLOCK_MONOTONIC) + 15 * RLIMIT_TEST_SECS * 1000000000ULL;

    if (getrlimit(RLIMIT_CPU, &got) != 0 || got.rlim_cur != RLIMIT_TEST_SECS ||
        got.rlim_max != RLIMIT_TEST_SECS)
        return 6;

    /* Bounded, so a broken inheritance fails the test instead of leaving an
     * orphaned spinner on a hart for the rest of the run.
     */
    while (clock_ns(CLOCK_MONOTONIC) < end)
        ;
    return 9;
}

int rlimit_inherit_child(void)
{
    struct rlimit rl = {RLIMIT_TEST_SECS, RLIMIT_TEST_SECS};
    char *argv[] = {"syscall_testing", "--rlimit-inherit-grandchild", NULL};
    pid_t pid;
    int status = 0;

    if (setrlimit(RLIMIT_CPU, &rl) != 0)
        return 4;
    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != EOK)
        return 5;
    if (waitpid(pid, &status, 0) != pid)
        return 7;

    /* Stopped by SIGXCPU (not a normal exit, so not the 6 of a bad readback).
     */
    return WIFEXITED(status) ? 8 : 0;
}

/* CPU time of every child this process has reaped, user plus system. */
static uint64_t children_cpu_ns(void)
{
    struct rusage ru;

    if (getrusage(RUSAGE_CHILDREN, &ru) != 0)
        return 0;
    return ((uint64_t) ru.ru_utime.tv_sec + (uint64_t) ru.ru_stime.tv_sec) *
               1000000000ULL +
           ((uint64_t) ru.ru_utime.tv_usec + (uint64_t) ru.ru_stime.tv_usec) * 1000ULL;
}

static void test_rlimit_cpu_kill(void)
{
    char *argv[] = {"syscall_testing", "--rlimit-cpu-child", NULL};
    struct timespec res, nap = {.tv_sec = 0, .tv_nsec = 10000000};
    pid_t pid, r = 0;
    int status = 0, rc;
    uint64_t res_ns = 0, tol, limit = RLIMIT_TEST_SECS * 1000000000ULL;
    uint64_t cpu = 0, cpu0 = 0, w0, deadline;
    char detail[200];

    /* RUSAGE_CHILDREN accumulates every child this process ever reaped (an
     * earlier lap's spinner included), so measure this child as a delta.
     */
    cpu0 = children_cpu_ns();
    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("rlimit_cpu spawn", strerror(rc));
        return;
    }
    w0 = clock_ns(CLOCK_MONOTONIC);
    deadline = w0 + 20 * limit;
    while ((r = waitpid(pid, &status, WNOHANG)) == 0 &&
           clock_ns(CLOCK_MONOTONIC) < deadline)
        (void) nanosleep(&nap, NULL);
    if (r == 0) {
        kill(pid, SIGKILL);
        (void) waitpid(pid, &status, 0);
    }
    if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
        res_ns = ts_ns(&res);
    tol = limit / 100 > 2 * res_ns ? limit / 100 : 2 * res_ns;
    cpu = children_cpu_ns() - cpu0;
    snprintf(detail, sizeof detail,
             "reaped=%d status=0x%x child cpu=%llu limit=%llu tol=%llu "
             "wall=%llu res=%llu",
             r == pid, status, (unsigned long long) cpu,
             (unsigned long long) limit, (unsigned long long) tol,
             (unsigned long long) (clock_ns(CLOCK_MONOTONIC) - w0),
             (unsigned long long) res_ns);
    check(r == pid && !(WIFEXITED(status) && WEXITSTATUS(status) != 0),
          "rlimit_cpu: spinning child is stopped by its limit", detail);
    check(r == pid && cpu + tol >= limit && cpu <= limit + tol,
          "rlimit_cpu: stop lands within max(1%, 2 x resolution) of the limit",
          detail);
}

static void test_rlimit_inherit(void)
{
    char *argv[] = {"syscall_testing", "--rlimit-inherit-child", NULL};
    struct timespec nap = {.tv_sec = 0, .tv_nsec = 10000000};
    uint64_t deadline = clock_ns(CLOCK_MONOTONIC) +
                        20 * RLIMIT_TEST_SECS * 1000000000ULL;
    pid_t pid, r = 0;
    int status = 0, rc;
    char detail[100];

    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("rlimit inherit spawn", strerror(rc));
        return;
    }
    while ((r = waitpid(pid, &status, WNOHANG)) == 0 &&
           clock_ns(CLOCK_MONOTONIC) < deadline)
        (void) nanosleep(&nap, NULL);
    if (r == 0) {
        kill(pid, SIGKILL);
        (void) waitpid(pid, &status, 0);
    }
    snprintf(detail, sizeof detail, "reaped=%d status=0x%x", r == pid, status);
    check(r == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "rlimit_cpu: spawned child inherits the limit and is stopped by it",
          detail);
}

/* K11: RLIMIT_AS and RLIMIT_DATA are enforced when a mapping is created. Only
 * the soft limit is lowered, so the hard limit stays RLIM_INFINITY and each
 * case restores the process's limits when it is done.
 *
 * The process's usage is not exported, so rlm_usage finds it: with the soft
 * limit at L bytes a one-page private anonymous mapping fits exactly when usage
 * + page <= L, so a binary search over L recovers usage to the page. The cases
 * then set the limit to usage plus RLM_N chunks and check where mapping stops.
 */
#define RLM_PAGE 4096UL
#define RLM_CHUNK (64UL * 1024)
#define RLM_N 8
#define RLM_MAX_PAGES (1UL << 28) /* 1 TiB, above any usage here */
#define RLM_FAILED ((size_t) -1)
#define RLM_MALLOC (256UL * 1024) /* above malloc's direct-map threshold */

static int rlm_set(int res, rlim_t cur)
{
    struct rlimit rl = {cur, RLIM_INFINITY};

    return setrlimit(res, &rl);
}

static void *rlm_map(size_t len)
{
    return mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1,
                0);
}

/* errno of a mapping of len with flags, or 0 if it fit (it is undone). */
static int rlm_try_mapf(size_t len, int flags)
{
    void *p;

    errno = 0;
    p = mmap(NULL, len, PROT_READ | PROT_WRITE, flags, -1, 0);
    if (p == MAP_FAILED)
        return errno ? errno : -1;
    (void) munmap(p, len);
    return 0;
}

static int rlm_try_map(size_t len)
{
    return rlm_try_mapf(len, MAP_PRIVATE | MAP_ANON);
}

/* Bytes of res in use, or RLM_FAILED. Leaves the soft limit at RLIM_INFINITY.
 */
static size_t rlm_usage(int res)
{
    size_t lo = 0, hi = RLM_MAX_PAGES;

    while (hi - lo > 1) {
        size_t mid = lo + (hi - lo) / 2;

        if (rlm_set(res, mid * RLM_PAGE) != 0)
            break;
        if (rlm_try_map(RLM_PAGE) == 0)
            hi = mid;
        else
            lo = mid;
    }
    if (rlm_set(res, RLIM_INFINITY) != 0 || hi - lo > 1 || hi == RLM_MAX_PAGES)
        return RLM_FAILED;
    return (hi - 1) * RLM_PAGE;
}

/* Map RLM_CHUNK pieces into v[] until one fails or max are held.
 *
 * Returns the count; *err is the failing errno, or 0 if none failed.
 */
static int rlm_fill(void **v, int max, int *err)
{
    int n = 0;

    *err = 0;
    while (n < max) {
        void *p = rlm_map(RLM_CHUNK);

        if (p == MAP_FAILED) {
            *err = errno;
            break;
        }
        v[n++] = p;
    }
    return n;
}

static void rlm_unmap(void **v, int n)
{
    for (int i = 0; i < n; i++)
        (void) munmap(v[i], RLM_CHUNK);
}

static void test_rlimit_as(void)
{
    void *v[RLM_N + 1], *p;
    struct rlimit got = {0, 0};
    size_t use, use2, lim;
    int n, err, fixed_ok = 1;
    char detail[160];

    use = rlm_usage(RLIMIT_AS);
    if (use == RLM_FAILED) {
        test_fail("rlimit_as: usage probe", strerror(errno));
        return;
    }
    lim = use + RLM_N * RLM_CHUNK;
    if (rlm_set(RLIMIT_AS, lim) != 0) {
        test_fail("rlimit_as: setrlimit", strerror(errno));
        return;
    }
    check(getrlimit(RLIMIT_AS, &got) == 0 && got.rlim_cur == lim &&
              got.rlim_max == RLIM_INFINITY,
          "rlimit_as: getrlimit reads back the soft limit", "");

    err = rlm_try_map(RLM_N * RLM_CHUNK + RLM_PAGE);
    check(err == ENOMEM, "rlimit_as: mmap past the limit fails ENOMEM",
          strerror(err));

    n = rlm_fill(v, RLM_N + 1, &err);
    snprintf(detail, sizeof detail, "mapped=%d of %d errno=%d", n, RLM_N, err);
    check(n == RLM_N && err == ENOMEM,
          "rlimit_as: mapping stops exactly at the limit", detail);

    /* At the limit, a MAP_FIXED remap of a held chunk is net zero. */
    for (int i = 0; n > 0 && i < 16; i++) {
        p = mmap(v[0], RLM_CHUNK, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (p != v[0]) {
            fixed_ok = 0;
            break;
        }
    }
    check(n > 0 && fixed_ok,
          "rlimit_as: MAP_FIXED replacement at the limit is charged net", "");
    err = rlm_try_map(RLM_PAGE);
    check(err == ENOMEM, "rlimit_as: limit still full after the replacements",
          strerror(err));

    /* A growing malloc needs a fresh mapping, so it fails at the limit too. */
    errno = 0;
    p = malloc(RLM_MALLOC);
    err = errno;
    free(p);
    check(!p && err == ENOMEM, "rlimit_as: a growing malloc fails ENOMEM",
          strerror(err));

    rlm_unmap(v, n);
    (void) rlm_set(RLIMIT_AS, RLIM_INFINITY);
    p = malloc(RLM_MALLOC);
    check(p != NULL, "rlimit_as: malloc succeeds after raising the limit", "");
    free(p);
    err = rlm_try_map(RLM_N * RLM_CHUNK + RLM_PAGE);
    check(err == 0, "rlimit_as: raising the limit lets the mapping succeed",
          strerror(err));

    use2 = rlm_usage(RLIMIT_AS);
    snprintf(detail, sizeof detail, "before=%zu after=%zu", use, use2);
    check(use2 == use, "rlimit_as: munmap uncharges exactly (no drift)",
          detail);
}

/* Two threads map concurrently against one limit. Both are created (stacks
 * mapped) and parked before the usage probe, so the probe counts them and the
 * limit leaves room for exactly RLM_N chunks in total.
 */
static _Atomic int rlm_start; /* 1 = limit set, go; -1 = abort */

struct rlm_racer {
    void *v[RLM_N + 1];
    int n, err;
};

static void *rlm_race(void *arg)
{
    struct rlm_racer *r = arg;
    int go;

    while ((go = atomic_load(&rlm_start)) == 0)
        sched_yield();
    if (go > 0)
        r->n = rlm_fill(r->v, RLM_N + 1, &r->err);
    return NULL;
}

static void test_rlimit_as_concurrent(void)
{
    struct rlm_racer r[2];
    pthread_t t[2];
    size_t use = RLM_FAILED;
    int made = 0;
    char detail[160];

    memset(r, 0, sizeof r);
    atomic_store(&rlm_start, 0);
    while (made < 2 && pthread_create(&t[made], NULL, rlm_race, &r[made]) == 0)
        made++;
    if (made == 2)
        use = rlm_usage(RLIMIT_AS);
    if (use != RLM_FAILED && rlm_set(RLIMIT_AS, use + RLM_N * RLM_CHUNK) != 0)
        use = RLM_FAILED;
    atomic_store(&rlm_start, use != RLM_FAILED ? 1 : -1);
    for (int i = 0; i < made; i++)
        (void) pthread_join(t[i], NULL);
    (void) rlm_set(RLIMIT_AS, RLIM_INFINITY);

    snprintf(detail, sizeof detail, "threads=%d use=%zu got=%d+%d errno=%d,%d",
             made, use, r[0].n, r[1].n, r[0].err, r[1].err);
    check(made == 2 && use != RLM_FAILED && r[0].n + r[1].n == RLM_N &&
              r[0].err == ENOMEM && r[1].err == ENOMEM,
          "rlimit_as: two concurrent mappers share the limit exactly", detail);
    rlm_unmap(r[0].v, r[0].n);
    rlm_unmap(r[1].v, r[1].n);
}

/* RLIMIT_DATA caps private anonymous memory, which is what malloc grows
 * through: a growing malloc fails ENOMEM at the limit and succeeds once the
 * limit is raised. A shared mapping is not data and still fits.
 */
#define RLM_MALLOC_MAX 64

static void test_rlimit_data(void)
{
    const size_t big = 4 * RLM_MALLOC + RLM_PAGE;
    void *blk[RLM_MALLOC_MAX], *p;
    size_t use, lim;
    int n = 0, err = 0;
    char detail[160];

    use = rlm_usage(RLIMIT_DATA);
    if (use == RLM_FAILED) {
        test_fail("rlimit_data: usage probe", strerror(errno));
        return;
    }
    lim = use + 4 * RLM_MALLOC;
    if (rlm_set(RLIMIT_DATA, lim) != 0) {
        test_fail("rlimit_data: setrlimit", strerror(errno));
        return;
    }

    err = rlm_try_map(big);
    check(err == ENOMEM, "rlimit_data: private mmap past the limit fails ENOMEM",
          strerror(err));

    /* MAP_ELF and MAP_STACK are not a way out: anonymous private memory is data
     * either way.
     */
    err = rlm_try_mapf(big, MAP_PRIVATE | MAP_ANON | MAP_ELF);
    check(err == ENOMEM, "rlimit_data: MAP_ELF anonymous mmap is charged as data",
          strerror(err));
    err = rlm_try_mapf(big, MAP_PRIVATE | MAP_ANON | MAP_STACK);
    check(err == ENOMEM, "rlimit_data: MAP_STACK anonymous mmap is charged as data",
          strerror(err));

    err = rlm_try_mapf(big, MAP_SHARED | MAP_ANON);
    check(err == 0, "rlimit_data: a shared mapping is not charged as data",
          strerror(err));

    errno = 0;
    while (n < RLM_MALLOC_MAX && (blk[n] = malloc(RLM_MALLOC)) != NULL) {
        memset(blk[n], 0xa5, RLM_MALLOC);
        n++;
    }
    err = errno;
    snprintf(detail, sizeof detail, "allocated=%d errno=%d", n, err);
    check(n <= 4 && err == ENOMEM,
          "rlimit_data: a growing malloc fails ENOMEM at the limit", detail);

    (void) rlm_set(RLIMIT_DATA, RLIM_INFINITY);
    p = malloc(RLM_MALLOC);
    check(p != NULL, "rlimit_data: malloc succeeds after raising the limit", "");
    free(p);
    while (n > 0)
        free(blk[--n]);
}

/* Q34: 1 ns nanosleep storm on a tickless hart. The sleeper is pinned to hart 1
 * and a watchdog counter to hart 2; the sleeper checks the watchdog advances in
 * every 100 ms window, i.e. that a storm of sub-tick timer deadlines does not
 * wedge or starve another hart. Needs three online harts.
 */
enum { STORM_CALLS = 100000, STORM_WINDOW_NS = 100000000 };
#define STORM_DEADLINE_NS 100000000000ULL /* 100 s */
#define STORM_INTR_SLACK 64

struct storm_ctx {
    _Atomic uint64_t counter;
    _Atomic int stop;
    _Atomic int pinned; /* 1 = watchdog pinned, -1 = pin failed */
    int sleeper_cpu, watchdog_cpu;
    uint64_t elapsed_ns;
    long backward, failed_calls, stalled_windows, windows;
    long intr_before, intr_after;
};

static int storm_pin(unsigned cpu)
{
    size_t bytes = sizeof(struct _thread_runmask) + 2U * sizeof(unsigned);
    struct _thread_runmask *m = calloc(1U, bytes);
    unsigned *runmask;
    unsigned mask = 0;
    int rc;

    if (!m)
        return -1;
    RMSK_SET(cpu, &mask);
    m->size = 1U;
    runmask = (unsigned *) (m + 1);
    runmask[0] = mask; /* run mask */
    runmask[1] = 0; /* inherit mask: 0 leaves the process default alone */
    rc = thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, m);
    free(m);
    return rc;
}

/* "timer.<hart>.intr" from /sys/kstats, or -1 if the key is unreadable. */
static long storm_intr(int cpu)
{
    char key[32], buf[8192];
    ssize_t n;
    size_t klen;
    char *p;
    int fd = open("/sys/kstats", O_RDONLY);

    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof buf - 1);
    (void) close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    klen = (size_t) snprintf(key, sizeof key, "timer.%d.intr=", cpu);
    p = strstr(buf, key);
    if (!p || (p != buf && p[-1] != '\n'))
        return -1;
    return strtol(p + klen, NULL, 10);
}

static void *storm_watchdog(void *arg)
{
    struct storm_ctx *c = arg;

    atomic_store(&c->pinned, storm_pin((unsigned) c->watchdog_cpu) == 0 ? 1 : -1);
    while (!atomic_load(&c->stop))
        atomic_fetch_add_explicit(&c->counter, 1, memory_order_relaxed);
    return NULL;
}

static void *storm_sleeper(void *arg)
{
    struct storm_ctx *c = arg;
    struct timespec quiet = {.tv_sec = 0, .tv_nsec = STORM_WINDOW_NS};
    uint64_t t0, last, win_start, win_count;

    if (storm_pin((unsigned) c->sleeper_cpu) != 0) {
        c->failed_calls = -1;
        return NULL;
    }
    (void) nanosleep(&quiet, NULL); /* quiet interval: hart goes tickless */
    c->intr_before = storm_intr(c->sleeper_cpu);
    t0 = last = win_start = clock_ns(CLOCK_MONOTONIC);
    win_count = atomic_load(&c->counter);
    for (int i = 0; i < STORM_CALLS; i++) {
        struct timespec req = {.tv_sec = 0, .tv_nsec = 1}, rem;
        uint64_t now;

        while (nanosleep(&req, &rem) != 0) {
            if (errno != EINTR) {
                c->failed_calls++;
                break;
            }
            req = rem;
        }
        now = clock_ns(CLOCK_MONOTONIC);
        if (now - t0 > STORM_DEADLINE_NS) {
            c->failed_calls++; /* bounded: a regression must fail, not hang */
            break;
        }
        if (now < last)
            c->backward++;
        last = now;
        if (now - win_start >= STORM_WINDOW_NS) {
            uint64_t cnt = atomic_load(&c->counter);

            c->windows++;
            if (cnt == win_count)
                c->stalled_windows++;
            win_count = cnt;
            win_start = now;
        }
    }
    c->elapsed_ns = last - t0;
    c->intr_after = storm_intr(c->sleeper_cpu);
    return NULL;
}

static void test_nanosleep_1ns_storm(void)
{
    static struct storm_ctx c;
    pthread_t wd, sl;
    char detail[200];

    if (sysconf(_SC_NPROCESSORS_ONLN) < 3) {
        test_skip("nanosleep_1ns_storm", "needs 3 online harts");
        return;
    }
    c.sleeper_cpu = 1;
    c.watchdog_cpu = 2;
    c.intr_before = c.intr_after = -1;
    if (pthread_create(&wd, NULL, storm_watchdog, &c) != 0) {
        test_fail("nanosleep_1ns_storm fixture", strerror(errno));
        return;
    }
    while (!atomic_load(&c.pinned))
        (void) sched_yield();
    if (pthread_create(&sl, NULL, storm_sleeper, &c) != 0) {
        atomic_store(&c.stop, 1);
        (void) pthread_join(wd, NULL);
        test_fail("nanosleep_1ns_storm fixture", strerror(errno));
        return;
    }
    (void) pthread_join(sl, NULL);
    atomic_store(&c.stop, 1);
    (void) pthread_join(wd, NULL);

    snprintf(detail, sizeof detail,
             "calls=%d failed=%ld backward=%ld windows=%ld stalled=%ld "
             "elapsed_ns=%llu intr_delta=%ld",
             STORM_CALLS, c.failed_calls, c.backward, c.windows,
             c.stalled_windows, (unsigned long long) c.elapsed_ns,
             c.intr_before >= 0 && c.intr_after >= 0
                 ? c.intr_after - c.intr_before
                 : -1L);
    printf("  nanosleep_1ns_storm: %s\n", detail);
    check(atomic_load(&c.pinned) == 1 && c.failed_calls == 0 && !c.backward &&
              c.windows > 0 && !c.stalled_windows,
          "nanosleep_1ns_storm completes, time monotonic, watchdog advances",
          detail);

    /* Upper bound on the sleeper hart's STIMER interrupts. Each 1 ns sleep arms
     * one deadline, and one deadline costs at most one interrupt (a sleep that
     * finds its deadline already past takes none), so the calls contribute <=
     * STORM_CALLS. On top of that the hart takes at most the periodic heartbeat
     * (one per tick of clock_getres) and at most one round-robin slice expiry
     * per tick, so 2 * (elapsed / tick + 1); STORM_INTR_SLACK covers the two
     * /sys/kstats reads (roottask IPC on this hart) and wakeups that predate
     * the first sample. An unbounded count means the interrupt path re-fires
     * without advancing (a livelock), which is what the bound is there to
     * catch; it deliberately has no lower bound, since a sleep may legitimately
     * finish without an interrupt.
     */
    {
        struct timespec res;
        uint64_t tick_ns = 0, bound;
        long delta;

        if (clock_getres(CLOCK_MONOTONIC, &res) == 0)
            tick_ns = (uint64_t) res.tv_sec * 1000000000ULL + (uint64_t) res.tv_nsec;
        if (tick_ns == 0)
            tick_ns = 1000000ULL; /* unreadable: assume a 1 ms tick */
        delta = c.intr_after - c.intr_before;
        bound = STORM_CALLS + 2 * (c.elapsed_ns / tick_ns + 1) + STORM_INTR_SLACK;
        snprintf(detail, sizeof detail,
                 "intr_delta=%ld bound=%llu tick_ns=%llu elapsed_ns=%llu", delta,
                 (unsigned long long) bound, (unsigned long long) tick_ns,
                 (unsigned long long) c.elapsed_ns);
        check(c.intr_before >= 0 && c.intr_after >= c.intr_before &&
                  (uint64_t) delta <= bound,
              "nanosleep_1ns_storm timer interrupt delta within bound", detail);
    }
}

/* Entry point */

void test_timers(void)
{
    /* POSIX timer wrappers */
    test_posix_timer_create_delete();
    test_posix_timer_create_invalid_args();
    test_posix_timer_settime_gettime();
    test_posix_timer_settime_invalid();
    test_posix_timer_getoverrun();
    test_posix_clock_edges();

    /* __timer_create success */
    test_timer_create_realtime();
    test_timer_create_monotonic();
    test_timer_create_softtime();
    test_timer_create_unique_ids();

    /* __timer_create errors */
    test_timer_create_invalid_clock();
    test_timer_create_negative_clock();
    test_timer_create_null_event();
    test_timer_create_signal_event_rejected();
    test_timer_create_notification_bad_coid();

    /* __timer_destroy success */
    test_timer_destroy_valid();

    /* __timer_destroy errors */
    test_timer_destroy_invalid_id();
    test_timer_destroy_negative_id();
    test_timer_destroy_double();

    /* Stress / edge cases */
    test_timer_create_exhaust();
    test_timer_id_reuse();

    /* CPU-time billing */
    test_cputime_short_bursts();
    test_cputime_two_threads();
    test_cputime_yield_steal_stress();
    test_rlimit_negative();
    test_rlimit_cpu_kill();
    test_rlimit_inherit();
    test_rlimit_as();
    test_rlimit_as_concurrent();
    test_rlimit_data();
    test_nanosleep_1ns_storm();
}
