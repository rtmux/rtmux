/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * prio-loan — a thread's own (base) priority versus priority lent to it.
 *
 * The kernel keeps each thread's base priority (what SchedSet sets) apart from
 * what other threads lend it: a PI-mutex waiter, a PRIO_PROTECT ceiling, the
 * client whose request it serves, or a send queued on its channel. It runs at
 * the maximum, and a loan ending returns it to its base, not to whatever
 * priority it ran at before. See docs/pi-pc-design.md "Base and Effective
 * Priority".
 *
 * Each case reads the priorities of sibling threads with __sched_get(pid 0,
 * tid): sched_priority is the base, sched_curpriority the effective priority.
 * The main thread runs at OBSERVER_PRIO while it polls so it can always look.
 * Every wait is bounded by POLL_MS, so a regression fails a case instead of
 * hanging the suite.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>

#include "test.h"

#define OBSERVER_PRIO 62
#define POLL_MS 3000 /* the longest any wait in a case may take */
#define DEPTH_MAX 64 /* the Kconfig range of CONFIG_PRIO_REPLY_DEPTH */

static int set_fifo(int prio)
{
    struct sched_param sp = {.sched_priority = prio};

    return __sched_set(0, 0, SCHED_FIFO, &sp);
}

/* Effective priority of sibling thread t (base in *base); -1 on error. */
static int prio_of(pthread_t t, int *base)
{
    struct sched_param sp;

    if (__sched_get(0, (int) t, &sp) == -1)
        return -1;
    if (base)
        *base = sp.sched_priority;
    return sp.sched_curpriority;
}

static int self_prio(int *base)
{
    return prio_of(pthread_self(), base);
}

/* Poll t for up to tries ms until its effective priority equals want; returns
 * the last value.
 */
static int wait_prio_n(pthread_t t, int want, int tries)
{
    int cur = -1;

    for (int i = 0; i < tries; i++) {
        cur = prio_of(t, NULL);
        if (cur == want)
            break;
        sleep_ms(1);
    }
    return cur;
}

static int wait_prio(pthread_t t, int want)
{
    return wait_prio_n(t, want, POLL_MS);
}

/* Wait at most POLL_MS for flag; returns whether it was set. */
static bool wait_flag(atomic_int *flag)
{
    for (int i = 0; i < POLL_MS; i++) {
        if (atomic_load_explicit(flag, memory_order_acquire))
            return true;
        sleep_ms(1);
    }
    return atomic_load_explicit(flag, memory_order_acquire) != 0;
}

static void set_flag(atomic_int *flag)
{
    atomic_store_explicit(flag, 1, memory_order_release);
}

static void abs_deadline(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static int pi_mutex_init(pthread_mutex_t *m)
{
    pthread_mutexattr_t attr;

    if (pthread_mutexattr_init(&attr) != EOK ||
        pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT) != EOK)
        return -1;
    return pthread_mutex_init(m, &attr) == EOK ? 0 : -1;
}

/* A channel and a connection to it in this process. */
static int chan_open(int *chid, int *coid)
{
    *coid = -1;
    *chid = channel_create(0);
    if (*chid == -1)
        return -1;
    *coid = connect_attach(0, getpid(), *chid, RTMUX_SIDE_CHANNEL, 0);
    return *coid == -1 ? -1 : 0;
}

static void chan_close(int chid, int coid)
{
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);
    if (chid != -1)
        (void) channel_destroy(chid);
}

/* pthread_create, reporting a failure against the case. */
static bool spawn(pthread_t *t, void *(*fn)(void *), void *arg, const char *what)
{
    int rc = pthread_create(t, NULL, fn, arg);

    if (rc != EOK)
        test_fail(what, strerror(rc));
    return rc == EOK;
}

/* A thread that locks a PI mutex at a priority with a timeout (POLL_MS when
 * none is given), records the result, and unlocks.
 */
struct locker {
    pthread_mutex_t *m;
    int prio;
    long timeout_ms;
    atomic_int done;
    int rc;
};

static void *locker_main(void *raw)
{
    struct locker *l = raw;
    struct timespec dl;

    (void) set_fifo(l->prio);
    abs_deadline(&dl, l->timeout_ms ? l->timeout_ms : POLL_MS);
    l->rc = pthread_mutex_timedlock(l->m, &dl);
    if (l->rc == EOK)
        (void) pthread_mutex_unlock(l->m);
    set_flag(&l->done);
    return NULL;
}

/* A client that sends one empty message at a priority, optionally holding a
 * PI mutex across the send, optionally with a REPLY timeout.
 */
struct client {
    int coid;
    int prio;
    pthread_mutex_t *hold; /* NULL: hold nothing */
    long reply_timeout_ms; /* 0: none */
    atomic_int done;
    int rc, err;
};

static void *client_main(void *raw)
{
    struct client *c = raw;
    char req = 0, rep = 0;

    (void) set_fifo(c->prio);
    if (c->hold)
        (void) pthread_mutex_lock(c->hold);
    if (c->reply_timeout_ms) {
        uint64_t ns = (uint64_t) c->reply_timeout_ms * 1000000ULL;

        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_REPLY, NULL, &ns,
                               NULL);
    }
    c->rc = msg_send(c->coid, &req, 1, &rep, 1);
    c->err = errno;
    if (c->hold)
        (void) pthread_mutex_unlock(c->hold);
    set_flag(&c->done);
    return NULL;
}

/* A server that receives one message, flags it, waits for `go`, replies, and
 * records its priorities just after receiving and after the reply.
 */
struct server {
    int chid;
    int base;
    pthread_mutex_t *hold; /* held from before the receive until `go` */
    atomic_int ready, received, go;
    int rcvid;
    int prio_recv, prio_unlocked, prio_after, base_after, reply_rc;
};

static void *server_main(void *raw)
{
    struct server *k = raw;
    char buf[1], z = 0;

    (void) set_fifo(k->base);
    if (k->hold)
        (void) pthread_mutex_lock(k->hold);
    set_flag(&k->ready);
    k->rcvid = msg_receive(k->chid, buf, sizeof buf, NULL);
    k->prio_recv = self_prio(NULL);
    set_flag(&k->received);
    (void) wait_flag(&k->go);
    if (k->hold)
        (void) pthread_mutex_unlock(k->hold);
    k->prio_unlocked = self_prio(NULL);
    k->reply_rc = k->rcvid == -1 ? -1 : msg_reply(k->rcvid, 0, &z, 1);
    k->prio_after = self_prio(&k->base_after);
    return NULL;
}

/* Case 1: a server that holds a PI mutex wanted at 50 receives a message at
 * 10. Before the split, the receive overwrote the PI boost with the client's
 * 10. It must stay at >= 50, and after unlock and reply be back at its base.
 */
#define C1_BASE 15

static void test_loan_mutex_and_message(void)
{
    static struct server k;
    pthread_mutex_t m;
    struct locker w = {.m = &m, .prio = 50};
    struct client c = {.prio = 10};
    pthread_t ts, tw, tc;
    int coid = -1;
    char detail[160];

    memset(&k, 0, sizeof k);
    if (pi_mutex_init(&m) != 0 || chan_open(&k.chid, &coid) != 0) {
        test_fail("prio loan: mutex + message setup", strerror(errno));
        return;
    }
    k.base = C1_BASE;
    k.hold = &m;
    c.coid = coid;
    if (!spawn(&ts, server_main, &k, "prio loan: mutex + message server"))
        return;
    (void) wait_flag(&k.ready);
    if (!spawn(&tw, locker_main, &w, "prio loan: mutex + message waiter"))
        return;
    snprintf(detail, sizeof detail, "server at %d, want 50 from the waiter",
             wait_prio(ts, 50));
    check(prio_of(ts, NULL) == 50, "prio loan: PI waiter boosts the server",
          detail);
    if (!spawn(&tc, client_main, &c, "prio loan: mutex + message client"))
        return;
    (void) wait_flag(&k.received);
    snprintf(detail, sizeof detail,
             "server ran at %d after receiving a priority-10 message", k.prio_recv);
    check(k.prio_recv >= 50,
          "prio loan: a message loan does not displace a PI loan", detail);
    set_flag(&k.go);
    (void) pthread_join(ts, NULL);
    (void) pthread_join(tw, NULL);
    (void) pthread_join(tc, NULL);
    snprintf(detail, sizeof detail,
             "after unlock %d (want %d); after reply %d base %d (want %d)",
             k.prio_unlocked, C1_BASE, k.prio_after, k.base_after, C1_BASE);
    check(k.prio_unlocked == C1_BASE && k.prio_after == C1_BASE &&
              k.base_after == C1_BASE,
          "prio loan: server returns to its base after unlock and reply", detail);
    chan_close(k.chid, coid);
    (void) pthread_mutex_destroy(&m);
}

/* Case 2: a priority-60 waiter on a mutex whose owner is REPLY-blocked on a
 * priority-10 server makes that server run at 60. The waiter uses a timeout:
 * when it gives up, the loan leaves the chain and the server falls back to the
 * 20 its client lends, and after the reply to its base.
 */
#define C2_BASE 10

static void test_loan_through_reply(void)
{
    static struct server k;
    pthread_mutex_t m;
    struct client o = {.prio = 20};
    struct locker h = {.m = &m, .prio = 60, .timeout_ms = 400};
    pthread_t tv, to, th;
    int coid = -1, cur;
    char detail[160];

    memset(&k, 0, sizeof k);
    if (pi_mutex_init(&m) != 0 || chan_open(&k.chid, &coid) != 0) {
        test_fail("prio loan: REPLY propagation setup", strerror(errno));
        return;
    }
    k.base = C2_BASE;
    o.coid = coid;
    o.hold = &m;
    if (!spawn(&tv, server_main, &k, "prio loan: REPLY propagation server"))
        return;
    (void) wait_flag(&k.ready);
    if (!spawn(&to, client_main, &o, "prio loan: REPLY propagation client"))
        return;
    (void) wait_flag(&k.received);
    cur = wait_prio(tv, 20);
    snprintf(detail, sizeof detail, "server at %d, want 20 from its client", cur);
    check(cur == 20, "prio loan: server runs at its client's priority", detail);

    if (!spawn(&th, locker_main, &h, "prio loan: REPLY propagation waiter"))
        return;
    cur = wait_prio(tv, 60);
    snprintf(detail, sizeof detail,
             "server at %d, want 60 from the waiter on its client's mutex", cur);
    check(cur == 60, "prio loan: PI boost propagates through REPLY", detail);

    (void) wait_flag(&h.done);
    cur = wait_prio(tv, 20);
    snprintf(detail, sizeof detail,
             "server at %d after the waiter timed out (rc %d), want 20", cur, h.rc);
    check(cur == 20 && h.rc == ETIMEDOUT,
          "prio loan: a timed-out waiter's loan leaves the REPLY chain", detail);

    set_flag(&k.go);
    (void) pthread_join(to, NULL);
    (void) pthread_join(tv, NULL);
    (void) pthread_join(th, NULL);
    snprintf(detail, sizeof detail, "after the reply %d base %d, want %d",
             k.prio_after, k.base_after, C2_BASE);
    check(k.prio_after == C2_BASE && k.base_after == C2_BASE,
          "prio loan: the reply ends the client's loan", detail);
    chan_close(k.chid, coid);
    (void) pthread_mutex_destroy(&m);
}

/* Case 3: SchedSet on a boosted thread lowers its base but keeps the effective
 * priority until the loan ends.
 */
struct case3 {
    pthread_mutex_t m;
    atomic_int locked, boosted;
    int base_mid, cur_mid, base_end, cur_end;
};

static void *case3_owner(void *raw)
{
    struct case3 *k = raw;

    (void) set_fifo(30);
    (void) pthread_mutex_lock(&k->m);
    set_flag(&k->locked);
    (void) wait_flag(&k->boosted);
    (void) set_fifo(5);
    k->cur_mid = self_prio(&k->base_mid);
    (void) pthread_mutex_unlock(&k->m);
    k->cur_end = self_prio(&k->base_end);
    return NULL;
}

static void test_schedset_keeps_loan(void)
{
    static struct case3 k;
    struct locker w = {.prio = 50};
    pthread_t to, tw;
    char detail[160];

    memset(&k, 0, sizeof k);
    if (pi_mutex_init(&k.m) != 0) {
        test_fail("prio loan: SchedSet setup", "mutex init");
        return;
    }
    w.m = &k.m;
    if (!spawn(&to, case3_owner, &k, "prio loan: SchedSet owner"))
        return;
    (void) wait_flag(&k.locked);
    if (!spawn(&tw, locker_main, &w, "prio loan: SchedSet waiter"))
        return;
    (void) wait_prio(to, 50);
    set_flag(&k.boosted);
    (void) pthread_join(to, NULL);
    (void) pthread_join(tw, NULL);
    snprintf(detail, sizeof detail,
             "boosted: base %d cur %d (want 5, 50); after unlock: base %d cur %d "
             "(want 5, 5)",
             k.base_mid, k.cur_mid, k.base_end, k.cur_end);
    check(k.base_mid == 5 && k.cur_mid == 50 && k.base_end == 5 && k.cur_end == 5,
          "prio loan: SchedSet lowers the base, the loan holds until it ends",
          detail);
    (void) pthread_mutex_destroy(&k.m);
}

/* Case 4: a cycle through IPC. X holds M and is REPLY-blocked on Y; Y then
 * locks M. Y can never be granted M, and the walk that would boost X comes
 * back to Y through X's REPLY, so the lock fails with EDEADLK instead of
 * blocking.
 */
struct case4 {
    pthread_mutex_t *m;
    int chid;
    atomic_int ready;
    int rc;
};

static void *case4_server(void *raw)
{
    struct case4 *k = raw;
    char buf[1], z = 0;
    struct timespec dl;
    int rcvid;

    (void) set_fifo(15);
    set_flag(&k->ready);
    rcvid = msg_receive(k->chid, buf, sizeof buf, NULL);
    abs_deadline(&dl, 1000);
    k->rc = pthread_mutex_timedlock(k->m, &dl);
    if (k->rc == EOK)
        (void) pthread_mutex_unlock(k->m);
    if (rcvid != -1)
        (void) msg_reply(rcvid, 0, &z, 1);
    return NULL;
}

static void test_reply_cycle_deadlock(void)
{
    static struct case4 k;
    pthread_mutex_t m;
    struct client x = {.prio = 20};
    pthread_t ty, tx;
    int coid = -1;
    char detail[96];

    memset(&k, 0, sizeof k);
    if (pi_mutex_init(&m) != 0 || chan_open(&k.chid, &coid) != 0) {
        test_fail("prio loan: REPLY cycle setup", strerror(errno));
        return;
    }
    k.m = &m;
    x.coid = coid;
    x.hold = &m;
    if (!spawn(&ty, case4_server, &k, "prio loan: REPLY cycle server"))
        return;
    (void) wait_flag(&k.ready);
    if (!spawn(&tx, client_main, &x, "prio loan: REPLY cycle client"))
        return;
    (void) pthread_join(ty, NULL);
    (void) pthread_join(tx, NULL);
    snprintf(detail, sizeof detail, "lock returned %d, want EDEADLK (%d)", k.rc,
             EDEADLK);
    check(k.rc == EDEADLK, "prio loan: a mutex/REPLY cycle returns EDEADLK",
          detail);
    chan_close(k.chid, coid);
    (void) pthread_mutex_destroy(&m);
}

/* A /sys/kstats value, or -1. */
static long kstats_value(const char *key)
{
    static char buf[8192];
    char want[64];
    long v = -1;
    ssize_t n;
    char *p;
    int fd = open("/sys/kstats", O_RDONLY);

    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof buf - 1);
    (void) close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    snprintf(want, sizeof want, "\n%s=", key);
    p = strstr(buf, want);
    if (p)
        v = strtol(p + strlen(want), NULL, 10);
    return v;
}

/* Case 5: the REPLY depth bound. A client holding M sends down a chain of
 * depth + 1 servers (depth = CONFIG_PRIO_REPLY_DEPTH, from /sys/kstats). A
 * waiter on M reaches the first `depth` of them; the last keeps the priority
 * it had, and the walk is counted in /sys/kstats.
 */
#define C5_BASE 5

struct case5 {
    int n;
    int chid[DEPTH_MAX + 1];
    int coid[DEPTH_MAX + 1];
    pthread_t tid[DEPTH_MAX + 1];
    atomic_int idx;
    atomic_int last_received, go;
};

static void *case5_server(void *raw)
{
    struct case5 *k = raw;
    int i = atomic_fetch_add(&k->idx, 1);
    char buf[1], z = 0, rep = 0;
    int rcvid;

    (void) set_fifo(C5_BASE);
    rcvid = msg_receive(k->chid[i], buf, sizeof buf, NULL);
    if (i + 1 < k->n) {
        (void) msg_send(k->coid[i + 1], &z, 1, &rep, 1);
    } else {
        set_flag(&k->last_received);
        (void) wait_flag(&k->go);
    }
    if (rcvid != -1)
        (void) msg_reply(rcvid, 0, &z, 1);
    return NULL;
}

static void test_reply_depth_bound(void)
{
    static struct case5 k;
    pthread_mutex_t m;
    struct client c0 = {.prio = 10};
    struct locker h = {.m = &m, .prio = 60, .timeout_ms = 500};
    pthread_t th0, th;
    long depth = kstats_value("prio.reply_depth");
    long before, after;
    int reached = 0, last = -1, started = 0;
    char detail[192];

    memset(&k, 0, sizeof k);
    if (depth < 1 || depth > DEPTH_MAX) {
        test_fail("prio loan: REPLY depth setup", "no prio.reply_depth in /sys/kstats");
        return;
    }
    k.n = (int) depth + 1;
    if (pi_mutex_init(&m) != 0) {
        test_fail("prio loan: REPLY depth setup", "mutex init");
        return;
    }
    for (int i = 0; i < k.n; i++) {
        if (chan_open(&k.chid[i], &k.coid[i]) != 0) {
            test_fail("prio loan: REPLY depth setup", strerror(errno));
            return;
        }
    }
    for (int i = 0; i < k.n; i++) {
        /* One at a time, so server i is the thread created i-th. */
        if (!spawn(&k.tid[i], case5_server, &k, "prio loan: REPLY depth server"))
            break;
        started++;
        for (int t = 0; t < POLL_MS && atomic_load(&k.idx) <= i; t++)
            sleep_ms(1);
        sleep_ms(2);
    }
    c0.coid = k.coid[0];
    c0.hold = &m;
    if (started == k.n && spawn(&th0, client_main, &c0, "prio loan: REPLY depth client") &&
        wait_flag(&k.last_received)) {
        before = kstats_value("prio.reply_depth_overflow");
        if (spawn(&th, locker_main, &h, "prio loan: REPLY depth waiter")) {
            (void) wait_prio(k.tid[depth - 1], 60);
            sleep_ms(20);
            for (int i = 0; i < depth; i++) {
                if (prio_of(k.tid[i], NULL) == 60)
                    reached++;
            }
            last = prio_of(k.tid[k.n - 1], NULL);
            after = kstats_value("prio.reply_depth_overflow");
            snprintf(detail, sizeof detail,
                     "%d of %ld servers within the bound at 60; server %d at %d "
                     "(want 10); overflows %ld -> %ld",
                     reached, depth, k.n, last, before, after);
            check(reached == depth && last == 10 && before >= 0 && after > before,
                  "prio loan: REPLY propagation stops at CONFIG_PRIO_REPLY_DEPTH",
                  detail);
            (void) wait_flag(&h.done);
            (void) pthread_join(th, NULL);
        }
        set_flag(&k.go);
        (void) pthread_join(th0, NULL);
    } else {
        test_fail("prio loan: REPLY depth chain", "the chain did not form");
        set_flag(&k.go);
    }
    for (int i = 0; i < started; i++)
        (void) pthread_join(k.tid[i], NULL);
    for (int i = 0; i < k.n; i++)
        chan_close(k.chid[i], k.coid[i]);
    (void) pthread_mutex_destroy(&m);
}

/* Case 6: a client that leaves REPLY by timeout. Its loan ends with its
 * departure: the unblock path drops the server's REPLY loan, so the server is
 * back at its base within a tick, not at its next reply or receive; that reply
 * then fails with ESRCH.
 */
#define C6_BASE 5

static void test_loan_after_client_timeout(void)
{
    static struct server k;
    struct client c = {.prio = 40, .reply_timeout_ms = 100};
    pthread_t ts, tc;
    int coid = -1, during = -1;
    char detail[192];

    memset(&k, 0, sizeof k);
    if (chan_open(&k.chid, &coid) != 0) {
        test_fail("prio loan: client timeout setup", strerror(errno));
        return;
    }
    k.base = C6_BASE;
    c.coid = coid;
    if (!spawn(&ts, server_main, &k, "prio loan: client timeout server"))
        return;
    (void) wait_flag(&k.ready);
    if (!spawn(&tc, client_main, &c, "prio loan: client timeout client"))
        return;
    (void) wait_flag(&k.received);
    (void) wait_flag(&c.done);
    /* Within a tick of the timeout; a few more for another hart's IPI. */
    during = wait_prio_n(ts, C6_BASE, 5);
    set_flag(&k.go);
    (void) pthread_join(ts, NULL);
    (void) pthread_join(tc, NULL);
    snprintf(detail, sizeof detail,
             "received at %d (want 40); client rc %d errno %d (want ETIMEDOUT); "
             "server %d within 5 ms of the timeout (want %d), reply rc %d, then "
             "%d base %d",
             k.prio_recv, c.rc, c.err, during, C6_BASE, k.reply_rc,
             k.prio_after, k.base_after);
    check(k.prio_recv == 40 && c.rc == -1 && c.err == ETIMEDOUT &&
              during == C6_BASE && k.reply_rc == -1 &&
              k.prio_after == C6_BASE && k.base_after == C6_BASE,
          "prio loan: a timed-out client's loan ends with the timeout", detail);
    chan_close(k.chid, coid);
}

/* Case 7: a server that names no current client with msg_current(0) has no
 * request to serve, so the loan its client lent ends at once.
 */
static void *case7_server(void *raw)
{
    struct server *k = raw;
    char buf[1], z = 0;

    (void) set_fifo(k->base);
    set_flag(&k->ready);
    k->rcvid = msg_receive(k->chid, buf, sizeof buf, NULL);
    k->prio_recv = self_prio(NULL);
    set_flag(&k->received);
    (void) msg_current(0);
    k->prio_unlocked = self_prio(NULL);
    set_flag(&k->go);
    k->reply_rc = k->rcvid == -1 ? -1 : msg_reply(k->rcvid, 0, &z, 1);
    k->prio_after = self_prio(&k->base_after);
    return NULL;
}

#define C7_BASE 5

static void test_loan_ends_at_msg_current_zero(void)
{
    static struct server k;
    struct client c = {.prio = 40, .reply_timeout_ms = POLL_MS};
    pthread_t ts, tc;
    int coid = -1;
    char detail[160];

    memset(&k, 0, sizeof k);
    if (chan_open(&k.chid, &coid) != 0) {
        test_fail("prio loan: msg_current(0) setup", strerror(errno));
        return;
    }
    k.base = C7_BASE;
    c.coid = coid;
    if (!spawn(&ts, case7_server, &k, "prio loan: msg_current(0) server"))
        return;
    (void) wait_flag(&k.ready);
    if (!spawn(&tc, client_main, &c, "prio loan: msg_current(0) client"))
        return;
    (void) wait_flag(&k.go);
    (void) pthread_join(ts, NULL);
    (void) pthread_join(tc, NULL);
    snprintf(detail, sizeof detail,
             "received at %d (want 40), %d after msg_current(0) (want %d), "
             "reply rc %d",
             k.prio_recv, k.prio_unlocked, C7_BASE, k.reply_rc);
    check(k.prio_recv == 40 && k.prio_unlocked == C7_BASE,
          "prio loan: msg_current(0) ends the client's loan", detail);
    chan_close(k.chid, coid);
}

/* Case 8: a client that leaves REPLY by timeout while its server is itself
 * REPLY-blocked on a second server. The first server, not running, falls back
 * to its base. (The second server keeps what it was lent until it replies: the
 * chain is not followed from the timeout path.)
 */
#define C8_BASE_A 10
#define C8_BASE_B 5

struct c8 {
    struct server a, b;
    int coid_b;
    atomic_int a_sent;
};

static void *c8_server_a(void *raw)
{
    struct c8 *k = raw;
    char buf[1], req = 0, rep = 0, z = 0;

    (void) set_fifo(k->a.base);
    set_flag(&k->a.ready);
    k->a.rcvid = msg_receive(k->a.chid, buf, sizeof buf, NULL);
    k->a.prio_recv = self_prio(NULL);
    set_flag(&k->a.received);
    set_flag(&k->a_sent);
    k->a.reply_rc = msg_send(k->coid_b, &req, 1, &rep, 1);
    if (k->a.rcvid != -1)
        (void) msg_reply(k->a.rcvid, 0, &z, 1);
    return NULL;
}

static void test_loan_ends_for_blocked_server(void)
{
    static struct c8 k;
    struct client c = {.prio = 40, .reply_timeout_ms = 300};
    pthread_t ta, tb, tc;
    int coid_a = -1, coid_b = -1, during = -1;
    char detail[192];

    memset(&k, 0, sizeof k);
    if (chan_open(&k.a.chid, &coid_a) != 0 ||
        chan_open(&k.b.chid, &coid_b) != 0) {
        test_fail("prio loan: chained timeout setup", strerror(errno));
        return;
    }
    k.a.base = C8_BASE_A;
    k.b.base = C8_BASE_B;
    k.coid_b = coid_b;
    c.coid = coid_a;
    if (!spawn(&tb, server_main, &k.b, "prio loan: chained timeout server B"))
        return;
    (void) wait_flag(&k.b.ready);
    if (!spawn(&ta, c8_server_a, &k, "prio loan: chained timeout server A"))
        return;
    (void) wait_flag(&k.a.ready);
    if (!spawn(&tc, client_main, &c, "prio loan: chained timeout client"))
        return;
    (void) wait_flag(&k.b.received);
    (void) wait_flag(&c.done);
    during = wait_prio_n(ta, C8_BASE_A, 5);
    set_flag(&k.b.go);
    (void) pthread_join(tb, NULL);
    (void) pthread_join(ta, NULL);
    (void) pthread_join(tc, NULL);
    snprintf(detail, sizeof detail,
             "server B at %d while the client waited (want 40); server A %d within "
             "5 ms of the timeout (want %d); client rc %d errno %d",
             k.b.prio_recv, during, C8_BASE_A, c.rc, c.err);
    check(k.b.prio_recv == 40 && c.rc == -1 && c.err == ETIMEDOUT &&
              during == C8_BASE_A,
          "prio loan: a timed-out client's loan ends on a blocked server", detail);
    chan_close(k.a.chid, coid_a);
    chan_close(k.b.chid, coid_b);
}

/* K19: a kcall that lowers the caller below a ready thread and then fails. The
 * switch happens inside the kcall, before the trap handler writes the result,
 * so the status has to be in the saved context by then. A debug-inject kernel
 * fails the next K19_ROUNDS lowering SchedSet calls of this process after they
 * have applied the change; the caller and a competitor are pinned to one hart,
 * so the competitor really runs before the caller returns.
 */
#define K19_ROUNDS 1000
#define K19_HIGH 40
#define K19_MID 30
#define K19_LOW 10

static atomic_int k19_ran;

static void *k19_competitor(void *raw)
{
    (void) raw;
    atomic_store_explicit(&k19_ran, 1, memory_order_release);
    return NULL;
}

/* Returns 0 when it ran (the verdict goes through check), or TC_EXIT_SKIP when
 * fault injection is not available.
 */
int sched_set_error_after_lowering(void)
{
    struct sched_param saved, low = {.sched_priority = K19_LOW};
    char pidarg[16], count[16], detail[160];
    int policy, bad = 0, not_switched = 0, setup = 0;
    unsigned mask = 0;

    if (inject_run("sched-set-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    policy = __sched_get(0, 0, &saved);
    RMSK_SET(0, &mask);
    if (policy == -1 || test_pin_mask(mask) == -1 || set_fifo(K19_HIGH) == -1) {
        test_fail("sched_set_error_after_lowering setup", strerror(errno));
        return 0;
    }
    snprintf(pidarg, sizeof pidarg, "%d", (int) getpid());
    snprintf(count, sizeof count, "%d", K19_ROUNDS);
    if (inject_run("fail-sched-set", pidarg, count) != 0) {
        test_fail("sched_set_error_after_lowering setup", "inject refused to arm");
        goto out;
    }

    for (int i = 0; i < K19_ROUNDS; i++) {
        pthread_t t;
        int rc, err, ran;

        atomic_store_explicit(&k19_ran, 0, memory_order_relaxed);
        if (!condloan_create(&t, K19_MID, k19_competitor, NULL)) {
            setup++;
            break;
        }
        rc = __sched_set(0, 0, RTMUX_SCHED_SETPRIO, &low);
        err = errno;
        ran = atomic_load_explicit(&k19_ran, memory_order_acquire);
        (void) set_fifo(K19_HIGH);
        (void) pthread_join(t, NULL);
        if (!ran)
            not_switched++;
        if (rc != -1 || err != EINVAL)
            bad++;
    }

    /* The competitor running proves a switch, not that it came before the
     * status write: the trap handler counts each error return that found its
     * caller already switched out.
     */
    if (inject_run("sched-set-displaced", count, NULL) != 0)
        not_switched++;
    snprintf(detail, sizeof detail,
             "%d of %d calls lost the error status (%d never switched out, "
             "%d not started)",
             bad, K19_ROUNDS, not_switched, setup);
    check(bad == 0 && not_switched == 0 && setup == 0,
          "sched_set_error_after_lowering status survives a switch", detail);

out:
    /* Leave no failure armed against this process. */
    (void) inject_run("fail-sched-set", pidarg, "0");
    (void) test_pin_mask(~0U);
    (void) __sched_set(0, 0, policy, &saved);
    return 0;
}

void test_prio_loan(void)
{
    struct sched_param saved;
    int policy = __sched_get(0, 0, &saved);

    if (policy == -1 || set_fifo(OBSERVER_PRIO) == -1) {
        test_skip("prio loan", "cannot run the observer at SCHED_FIFO 62");
        return;
    }
    test_loan_mutex_and_message();
    test_loan_through_reply();
    test_schedset_keeps_loan();
    test_reply_cycle_deadlock();
    test_reply_depth_bound();
    test_loan_after_client_timeout();
    test_loan_ends_at_msg_current_zero();
    test_loan_ends_for_blocked_server();
    (void) __sched_set(0, 0, policy, &saved);
}
