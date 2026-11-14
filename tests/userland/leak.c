/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * leak.c — resource-leak gate: spawn/SIGKILL/reap rounds that must return the
 * kernel object gauges in /sys/kstats to baseline, and the between-lap gauge
 * comparison used by `syscall_testing --laps N`.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/dispatch.h>
#include <sys/rtmux-core.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test.h"

enum {
    KS_BUF = 4096,           /* one /sys/kstats snapshot */
    KS_POOLS = 32,           /* soul.<pool>.used gauges tracked across laps */
    KS_SETTLE_MS = 2000,     /* a gauge must reach baseline within this */
    KS_LATE_MS = 8000,       /* extra wait after a failure, to classify it */
    KS_POLL_MS = 10,
    LAP_BASELINE = 2,        /* lap whose end records the baseline; lap 1 warms pools */
    KILLSPAWN_WARMUP = 10,
    KILLSPAWN_ROUNDS = 1000,
};

static void ms_sleep(int ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};

    (void) nanosleep(&ts, NULL);
}

/* Resample a snapshot on the same descriptor; see docs/kstats.md. */
static int kstats_read(int fd, char *buf, size_t cap)
{
    ssize_t n, total = 0;

    if (lseek(fd, 0, SEEK_SET) != 0)
        return -1;
    while (total < (ssize_t) cap - 1 &&
           (n = read(fd, buf + total, cap - 1 - (size_t) total)) > 0)
        total += n;
    buf[total < 0 ? 0 : total] = '\0';
    return (int) total;
}

/* Value of "key" in a snapshot, or -1 if absent. */
static long kstats_get(const char *snap, const char *key)
{
    size_t klen = strlen(key);

    for (const char *p = snap; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, klen) && p[klen] == '=')
            return strtol(p + klen + 1, NULL, 10);
    return -1;
}

/* ---- killspawn ---------------------------------------------------------- */

static const char *const gauges[] = {"thread", "connect", "channel", "process"};
#define NGAUGE (sizeof gauges / sizeof gauges[0])

static void *killspawn_idle(void *arg)
{
    atomic_store((atomic_int *) arg, 1);
    for (;;)
        ms_sleep(1000);
    return NULL;
}

/* Child: hold a file, a second thread that has started running, and a
 * connection to the parent's channel, tell the parent through the inherited
 * pipe, then wait to be killed. Waiting for the thread to run keeps the round
 * about teardown of a live process: a kill landing on a thread still being
 * created has occasionally left soul.thread.used one high.
 */
int killspawn_child(const char *fd_arg)
{
    static atomic_int started;
    pthread_t th;
    int pfd = atoi(fd_arg);
    int file = open("/bin/true", O_RDONLY);
    int conn = name_open("killspawn", 0);

    if (file == -1 || conn == -1 || pthread_create(&th, NULL, killspawn_idle, &started) != 0)
        return EXIT_FAILURE;
    while (!atomic_load(&started))
        ms_sleep(1);
    if (write(pfd, "x", 1) != 1)
        return EXIT_FAILURE;
    for (;;)
        ms_sleep(1000);
}

/* Server dispatch pools (roottask, the pipe server) add a thread when requests
 * overlap and drop one only past their high-water mark, so soul.thread.used can
 * rise by a few for good without any thread being leaked (Q64: every thread of
 * the killed children was freed; the extra soul was a pool thread created while
 * a child spawned). Gate the thread gauge on the souls no live process owns:
 * soul.thread.used minus thread.live (threads in some process's thread vector).
 */
static long pool_adjust(const char *snap, const char *name, long used)
{
    /* A thread's timeout timer is allocated on its first timed wait, so the
     * timers of live threads depend on which pool thread timed out yet: subtract
     * them (timeout.live) and keep the timers nobody owns.
     */
    long live = kstats_get(snap, !strcmp(name, "soul.timer.used") ? "timeout.live"
                                                                  : "thread.live");

    if (strcmp(name, "soul.thread.used") && strcmp(name, "soul.timer.used"))
        return used;
    return live < 0 ? used : used - live;
}

static int gauge_sample(int fd, long *out)
{
    char snap[KS_BUF], key[48];

    if (kstats_read(fd, snap, sizeof snap) <= 0)
        return -1;
    for (size_t i = 0; i < NGAUGE; i++) {
        snprintf(key, sizeof key, "soul.%s.used", gauges[i]);
        if ((out[i] = kstats_get(snap, key)) < 0)
            return -1;
        out[i] = pool_adjust(snap, key, out[i]);
    }
    return 0;
}

/* One ack pipe serves every round, and the child inherits its write end, so the
 * rounds exercise the kill/reap path rather than pipe() setup and teardown
 * (test_pipe_gauge covers that).
 */
enum { ACK_TIMEOUT_MS = 10000 };

/* Spawn the child, wait for its ack, SIGKILL it and reap it. NULL on success. */
static const char *killspawn_round(const int *pfd)
{
    char fdarg[16], byte;
    char *argv[] = {"syscall_testing", "--killspawn-child", fdarg, NULL};
    const char *why = NULL;
    pid_t pid;
    int status, waited;

    snprintf(fdarg, sizeof fdarg, "%d", pfd[1]);
    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0)
        return "spawn";
    for (waited = 0; read(pfd[0], &byte, 1) != 1; waited += KS_POLL_MS) {
        if (waited >= ACK_TIMEOUT_MS) {
            why = "child did not acknowledge";
            break;
        }
        ms_sleep(KS_POLL_MS);
    }
    if (kill(pid, SIGKILL) != 0)
        why = why ? why : "kill";
    /* RTMux may report a SIGKILLed child as exit status 128 + SIGKILL. */
    if (waitpid(pid, &status, 0) != pid ||
        !(WIFSIGNALED(status) || (WIFEXITED(status) && WEXITSTATUS(status) == 128 + SIGKILL)))
        why = why ? why : "child was not reaped as SIGKILLed";
    return why;
}

/* Poll until every gauge equals base; on timeout report the first that differs
 * and return its index, else -1.
 */
static int gauges_settle(int fd, const long *base, long *now)
{
    for (int waited = 0;; waited += KS_POLL_MS) {
        int bad = -1;

        if (gauge_sample(fd, now) != 0)
            return 0;
        for (size_t i = 0; i < NGAUGE && bad < 0; i++)
            if (now[i] != base[i])
                bad = (int) i;
        if (bad < 0 || waited >= KS_SETTLE_MS)
            return bad;
        ms_sleep(KS_POLL_MS);
    }
}

/* name_attach() makes a CHF_DISCONNECT channel: the server side of each client
 * connection lives until the owner receives the disconnect notice and detaches
 * its scoid. The parent has no dispatch loop, so this thread plays that part.
 */
enum { DRAIN_POLL_NS = 100000000 };

struct drain {
    int chid;
    atomic_int stop;
};

static void *killspawn_drain(void *arg)
{
    struct drain *d = arg;

    while (!atomic_load(&d->stop)) {
        struct _notification n;
        uint64_t timeout_ns = DRAIN_POLL_NS;

        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);
        if (msg_receive_notification(d->chid, &n, sizeof n, NULL) == 0 &&
            n.code == _NOTIFICATION_CODE_DISCONNECT)
            (void) __connect_detach((uint64_t) n.scoid);
    }
    return NULL;
}

static void test_killspawn(void)
{
    long base[NGAUGE], prev[NGAUGE], now[NGAUGE];
    name_attach_t *attach;
    const char *why = NULL;
    char msg[160];
    int pfd[2] = {-1, -1};
    struct drain drain = {.chid = -1};
    pthread_t drain_th;
    int draining = 0;
    int fd, round, bad = -1, stable = 0;

    fd = open("/sys/kstats", O_RDONLY);
    attach = name_attach(NULL, "killspawn", 0);
    if (fd < 0 || !attach || pipe(pfd) != 0 || fcntl(pfd[0], F_SETFL, O_NONBLOCK) != 0) {
        test_fail("killspawn fixture", strerror(errno));
        goto out;
    }

    drain.chid = attach->chid;
    if (pthread_create(&drain_th, NULL, killspawn_drain, &drain) != 0) {
        test_fail("killspawn fixture", "drain thread");
        goto out;
    }
    draining = 1;

    for (round = 0; round < KILLSPAWN_WARMUP && !why; round++)
        why = killspawn_round(pfd);
    /* Baseline: three consecutive equal samples after the warm-up rounds. */
    memset(prev, -1, sizeof prev);
    for (int i = 0; i < KS_SETTLE_MS / KS_POLL_MS && stable < 3 && !why; i++) {
        if (gauge_sample(fd, base) != 0) {
            why = "kstats unreadable";
            break;
        }
        stable = memcmp(base, prev, sizeof base) ? 0 : stable + 1;
        memcpy(prev, base, sizeof prev);
        ms_sleep(KS_POLL_MS);
    }
    if (!why && stable < 3)
        why = "gauges did not settle after warm-up";
    if (why) {
        test_fail("killspawn warm-up", why);
        goto out;
    }

    for (round = 0; round < KILLSPAWN_ROUNDS; round++) {
        if ((why = killspawn_round(pfd)))
            break;
        bad = gauges_settle(fd, base, now);
        if (bad >= 0) {
            printf("killspawn: gauge %s base=%ld now=%ld round=%d\n", gauges[bad],
                   base[bad], now[bad], round);
            /* Tell a slow teardown from a leak: wait longer, report both. */
            for (int w = 0; w < KS_LATE_MS && gauge_sample(fd, now) == 0 && now[bad] != base[bad];
                 w += KS_POLL_MS)
                ms_sleep(KS_POLL_MS);
            printf("killspawn: gauge %s %s after a further %d ms (now=%ld)\n", gauges[bad],
                   now[bad] == base[bad] ? "recovered" : "still diverged", KS_LATE_MS,
                   now[bad]);
            break;
        }
    }
    if (why) {
        snprintf(msg, sizeof msg, "round %d: %s", round, why);
        test_fail("killspawn", msg);
    } else if (bad >= 0) {
        test_fail("killspawn", "gauge did not return to baseline");
    } else {
        test_ok("killspawn 1000 rounds return thread/connect/channel/process to baseline");
    }
out:
    if (draining) {
        atomic_store(&drain.stop, 1);
        (void) pthread_join(drain_th, NULL);
    }
    if (pfd[0] >= 0) {
        (void) close(pfd[0]);
        (void) close(pfd[1]);
    }
    if (attach)
        (void) name_detach(attach, 0);
    if (fd >= 0)
        (void) close(fd);
}

void test_leak(void)
{
    test_killspawn();
}

/* ---- per-operation gauge regressions ------------------------------------- */

enum { GAUGE_PAIRS = 100, GAUGE_CHILDREN = 10, GAUGE_FAILED_SPAWNS = 8 };

/* Value of "key" once it equals base, or the last value seen after
 * KS_SETTLE_MS: process, thread and connection teardown is asynchronous.
 */
static long gauge_settle(int fd, const char *key, long base)
{
    char snap[KS_BUF];
    long v = -1;

    for (int waited = 0; waited <= KS_SETTLE_MS; waited += KS_POLL_MS) {
        if (kstats_read(fd, snap, sizeof snap) <= 0)
            return -1;
        if ((v = kstats_get(snap, key)) == base)
            break;
        ms_sleep(KS_POLL_MS);
    }
    return v;
}

/* Baseline: the first value that two samples in a row agree on. */
static long gauge_stable(int fd, const char *key)
{
    char snap[KS_BUF];
    long prev = -1, v = -1;

    for (int waited = 0; waited <= KS_SETTLE_MS; waited += KS_POLL_MS) {
        if (kstats_read(fd, snap, sizeof snap) <= 0)
            return -1;
        v = kstats_get(snap, key);
        if (v == prev)
            break;
        prev = v;
        ms_sleep(KS_POLL_MS);
    }
    return v;
}

static void gauge_check(int fd, const char *what, const char *key, long base)
{
    char name[64], detail[64];
    long now = gauge_settle(fd, key, base);

    snprintf(name, sizeof name, "%s keeps %s at baseline", what, key);
    snprintf(detail, sizeof detail, "base=%ld now=%ld", base, now);
    check(now == base, name, detail);
}

/* GAUGE_PAIRS pipe()/close() pairs. Every pair used to leave one connect to the
 * pipe server behind: closing the last descriptor of a connection never told
 * the server, which therefore never detached its end. The pipe server's worker
 * pool grows on demand, so soul.thread.used is deliberately not compared.
 */
static void test_pipe_gauge(int fd)
{
    int pfd[2], made = 0;
    long connect0, channel0, process0;

    for (int i = 0; i < 8 && pipe(pfd) == 0; i++) { /* let the server pool settle */
        (void) close(pfd[0]);
        (void) close(pfd[1]);
    }
    connect0 = gauge_stable(fd, "soul.connect.used");
    channel0 = gauge_stable(fd, "soul.channel.used");
    process0 = gauge_stable(fd, "soul.process.used");
    while (made < GAUGE_PAIRS && pipe(pfd) == 0) {
        made++;
        (void) close(pfd[0]);
        (void) close(pfd[1]);
    }
    check(made == GAUGE_PAIRS, "pipe-gauge pipe() pairs", strerror(errno));
    gauge_check(fd, "100 pipe()/close() pairs", "soul.connect.used", connect0);
    gauge_check(fd, "100 pipe()/close() pairs", "soul.channel.used", channel0);
    gauge_check(fd, "100 pipe()/close() pairs", "soul.process.used", process0);
}

/* Children that sleep once and exit, reaped with waitpid, and spawns that fail
 * to load: none may leave a process or a per-thread timeout timer behind. A
 * failed spawn used to stay a zombie, a waitpid that won the race against the
 * last thread's exit left the reaped child pinned, and a thread that exited
 * after sleeping never freed its timeout timer.
 */
/* One batch: spawn, wait and reap GAUGE_CHILDREN children, then try a spawn and
 * an exec of a missing binary GAUGE_FAILED_SPAWNS times. *ok counts children
 * that exited 0, *failed the attempts that failed as they must.
 */
static void spawn_batch(int *ok, int *failed)
{
    char *nap_argv[] = {"syscall_testing", "--gauge-nap", NULL};
    char *bad_argv[] = {"x", NULL};

    *ok = *failed = 0;
    for (int i = 0; i < GAUGE_CHILDREN; i++) {
        pid_t pid;
        int status;

        if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, nap_argv, NULL) == 0 &&
            waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status))
            (*ok)++;
    }
    for (int i = 0; i < GAUGE_FAILED_SPAWNS; i++) {
        pid_t pid;

        if (i & 1 ? posix_spawn(&pid, "/rtmux-no-such-binary", NULL, NULL, bad_argv, NULL) != 0
                  : execv("/rtmux-no-such-binary", bad_argv) == -1)
            (*failed)++;
    }
}

static void test_spawn_gauge(int fd)
{
    long process0, timer0;
    int ok, failed;

    char snap[KS_BUF];
    long pa0 = -1, pa1 = -1;

    spawn_batch(&ok, &failed); /* warm up */
    process0 = gauge_stable(fd, "soul.process.used");
    timer0 = gauge_stable(fd, "soul.timer.used");
    if (kstats_read(fd, snap, sizeof snap) > 0)
        pa0 = kstats_get(snap, "pa.free_bytes");
    spawn_batch(&ok, &failed);
    if (kstats_read(fd, snap, sizeof snap) > 0)
        pa1 = kstats_get(snap, "pa.free_bytes");
    /* Physical memory is not a soul pool: report what a batch kept. */
    printf("spawn-gauge: pa.free_bytes %ld -> %ld (%ld bytes per child)\n", pa0, pa1,
           (pa0 - pa1) / GAUGE_CHILDREN);
    check(ok == GAUGE_CHILDREN, "spawn-gauge nap children exit 0", "child failed or was not reaped");
    check(failed == GAUGE_FAILED_SPAWNS, "spawn-gauge missing binary fails",
          "spawn or exec of a missing binary must fail");
    gauge_check(fd, "spawn, nap, exit, reap", "soul.process.used", process0);
    gauge_check(fd, "spawn, nap, exit, reap", "soul.timer.used", timer0);
}

/* ---- fixtures for the gauge regressions ---------------------------------- */

struct dr_state {
    atomic_int coid;
    atomic_int stop;
};

static void *dr_sender(void *arg)
{
    struct dr_state *d = arg;
    char out[8] = "ping", in[8];

    while (!atomic_load(&d->stop)) {
        int c = atomic_load(&d->coid);

        if (c >= 0)
            (void) msg_send(c, out, sizeof out, in, sizeof in);
        else
            sched_yield();
    }
    return NULL;
}

enum { DR_SENDERS = 3, DR_ITERS = 100, LINGER_THREADS = 6 };

/* Sibling threads MsgSend on a coid while the main thread detaches it (the last
 * reference, so the server gets its disconnect notice) and attaches a fresh one.
 */
static int detach_race_child(pid_t server, int chid)
{
    static struct dr_state d;
    pthread_t th[DR_SENDERS];
    int started = 0, rc = EXIT_SUCCESS;

    atomic_store(&d.coid, -1);
    for (; started < DR_SENDERS; started++)
        if (pthread_create(&th[started], NULL, dr_sender, &d) != 0)
            return EXIT_FAILURE;
    for (int i = 0; i < DR_ITERS; i++) {
        int c = connect_attach(0, server, chid, 0, 0), old;

        if (c < 0) {
            rc = EXIT_FAILURE;
            break;
        }
        atomic_store(&d.coid, c);
        if (i % 5 == 0)
        for (int y = 0; y < 20; y++)
            sched_yield();
        /* Drop the only reference from under senders that already loaded the
         * number: the race the last-drop notice has to survive.
         */
        old = atomic_exchange(&d.coid, -1);
        (void) __connect_detach((uint64_t) old);
    }
    atomic_store(&d.stop, 1);
    for (int i = 0; i < started; i++)
        (void) pthread_join(th[i], NULL);
    return rc;
}

/* A timed MsgSend whose reply the parent withholds: the child is killed with
 * its reply timeout armed.
 */
static int timed_send_child(pid_t server, int chid)
{
    uint64_t ns = 60ULL * 1000000000ULL;
    char out[8] = "ping", in[8];
    int c = connect_attach(0, server, chid, 0, 0);

    if (c < 0)
        return EXIT_FAILURE;
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_REPLY, NULL, &ns, NULL);
    (void) msg_send(c, out, sizeof out, in, sizeof in);
    return EXIT_SUCCESS;
}

struct orphan {
    int coid;
};

static void *orphan_sender(void *arg)
{
    struct orphan *o = arg;
    char out[8] = "ping", in[8];

    (void) msg_send(o->coid, out, sizeof out, in, sizeof in);
    return NULL;
}

/* One thread blocks in MsgSend; the main thread then closes the only descriptor
 * and waits to be killed. The sender's reference is then the last one, so the
 * dying thread's drop must finish the connection (Q65 b).
 */
static int orphan_send_child(pid_t server, int chid)
{
    static struct orphan o;
    pthread_t th;

    o.coid = connect_attach(0, server, chid, 0, 0);
    if (o.coid < 0 || pthread_create(&th, NULL, orphan_sender, &o) != 0)
        return EXIT_FAILURE;
    ms_sleep(100); /* the parent sees the message arrive; this only orders the close after it */
    (void) __connect_detach((uint64_t) o.coid);
    for (;;)
        ms_sleep(1000);
}

static void *linger_thread(void *arg)
{
    (void) arg;
    ms_sleep(1000);
    return NULL;
}

/* Many sleeping threads, so the process takes a while to lose its last one
 * after exit() has already posted its status.
 */
static int linger_child(void)
{
    pthread_t th;

    for (int i = 0; i < LINGER_THREADS; i++)
        if (pthread_create(&th, NULL, linger_thread, NULL) != 0)
            return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

/* Detach from the parent (NOZOMBIE), then exit a moment later. */
static int detach_exit_child(void)
{
    if (rtmux_detach(EXIT_SUCCESS) == -1)
        return EXIT_FAILURE;
    ms_sleep(20);
    return EXIT_SUCCESS;
}

int gauge_child(int argc, char **argv)
{
    pid_t server = argc > 3 ? (pid_t) atoi(argv[2]) : 0;
    int chid = argc > 3 ? atoi(argv[3]) : -1;

    if (!strcmp(argv[1], "--gauge-nap")) { /* sleep once: arms this thread's timeout timer */
        ms_sleep(5);
        return EXIT_SUCCESS;
    }
    if (!strcmp(argv[1], "--gauge-detach-race"))
        return detach_race_child(server, chid);
    if (!strcmp(argv[1], "--gauge-timed-send"))
        return timed_send_child(server, chid);
    if (!strcmp(argv[1], "--gauge-orphan-send"))
        return orphan_send_child(server, chid);
    if (!strcmp(argv[1], "--gauge-linger"))
        return linger_child();
    if (!strcmp(argv[1], "--gauge-detach-exit"))
        return detach_exit_child();
    return EXIT_FAILURE;
}

/* A server thread: answer every message at once until the channel is destroyed.
 * With held set, answer none: park the first rcvid in *held and fail it once
 * stop is raised.
 */
struct srv {
    int chid;
    atomic_int *held;
    atomic_int stop;
};

static void *srv_thread(void *arg)
{
    struct srv *sv = arg;
    union {
        char msg[16];
        struct _notification n;
    } buf;

    for (;;) {
        int rcvid = msg_receive(sv->chid, &buf, sizeof buf, NULL);

        if (rcvid < 0)
            return NULL;
        if (rcvid == 0) { /* a notification may be delivered here too */
            if (buf.n.code == _NOTIFICATION_CODE_DISCONNECT)
                (void) __connect_detach((uint64_t) buf.n.scoid);
            continue;
        }
        if (sv->held) {
            atomic_store(sv->held, rcvid);
            while (!atomic_load(&sv->stop))
                ms_sleep(1);
            (void) msg_error(rcvid, EINTR);
            return NULL;
        }
        (void) msg_reply(rcvid, 0, NULL, 0);
    }
}

/* The disconnect notices of a CHF_DISCONNECT channel: detach each scoid. */
static void *srv_drain(void *arg)
{
    struct srv *sv = arg;

    while (!atomic_load(&sv->stop)) {
        struct _notification n;
        uint64_t timeout_ns = 50000000ULL;

        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);
        if (msg_receive_notification(sv->chid, &n, sizeof n, NULL) == 0 &&
            n.code == _NOTIFICATION_CODE_DISCONNECT)
            (void) __connect_detach((uint64_t) n.scoid);
    }
    return NULL;
}

static pid_t gauge_spawn(const char *flag, int chid)
{
    char pidarg[16], chidarg[16];
    char *argv[] = {"syscall_testing", (char *) flag, pidarg, chidarg, NULL};
    pid_t pid;

    snprintf(pidarg, sizeof pidarg, "%d", (int) getpid());
    snprintf(chidarg, sizeof chidarg, "%d", chid);
    return posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) == 0 ? pid : -1;
}

static int reaped_clean(pid_t pid)
{
    int status;

    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == EXIT_SUCCESS;
}

/* A multi-thread client detaches and re-attaches a coid while its siblings
 * MsgSend on it, against a CHF_DISCONNECT server and a plain one. The last
 * detach of a coid sends the disconnect notice (CHF_DISCONNECT) while a sibling
 * may be between looking the connection up and publishing itself as a sender:
 * ksys_msg_sendv takes its connection reference before publishing, so that send
 * either holds the last reference (and is replied to or swept) or fails with
 * EBADF. The test pins that this stays crash-free (no links underflow), strands
 * no sender, reaps the child, and leaves every gauge at baseline once the
 * channel is gone. Only an SMP run can interleave the detach with a send; at
 * CPUS=1 it still covers the single-hart orderings.
 */
static void test_detach_race(int fd)
{
    static const unsigned flags[2] = {RTMUX_FLG_CHF_DISCONNECT, 0};
    static const char *const names[2] = {"detach-race CHF_DISCONNECT", "detach-race plain"};

    for (int k = 0; k < 2; k++) {
        long connect0 = gauge_stable(fd, "soul.connect.used");
        long channel0 = gauge_stable(fd, "soul.channel.used");
        long process0 = gauge_stable(fd, "soul.process.used");
        struct srv sv = {.chid = channel_create(flags[k])};
        pthread_t tsrv[2];
        int nsrv = 0;
        pid_t pid;

        if (sv.chid < 0) {
            test_fail("detach-race channel_create", strerror(errno));
            return;
        }
        for (int i = 0; i < 2; i++)
            if (pthread_create(&tsrv[nsrv], NULL, i ? srv_drain : srv_thread, &sv) == 0)
                nsrv++;
        pid = gauge_spawn("--gauge-detach-race", sv.chid);
        check(reaped_clean(pid), names[k], "multi-thread client crashed or was not reaped");
        atomic_store(&sv.stop, 1);
        (void) channel_destroy(sv.chid);
        for (int i = 0; i < nsrv; i++)
            (void) pthread_join(tsrv[i], NULL);
        gauge_check(fd, names[k], "soul.connect.used", connect0);
        gauge_check(fd, names[k], "soul.channel.used", channel0);
        gauge_check(fd, names[k], "soul.process.used", process0);
    }
}

/* A process killed while its thread waits in a timed MsgSend (reply timeout
 * armed) must free that timer, not crash or leak it.
 */
static void test_timed_send_kill(int fd)
{
    struct srv sv = {.chid = channel_create(0)};
    atomic_int held = 0;
    pthread_t th;
    long timer0, process0;
    pid_t pid;
    int status, waited = 0;

    if (sv.chid < 0) {
        test_fail("timed-send channel_create", strerror(errno));
        return;
    }
    sv.held = &held;
    timer0 = gauge_stable(fd, "soul.timer.used");
    process0 = gauge_stable(fd, "soul.process.used");
    if (pthread_create(&th, NULL, srv_thread, &sv) != 0) {
        test_fail("timed-send server thread", strerror(errno));
        (void) channel_destroy(sv.chid);
        return;
    }
    pid = gauge_spawn("--gauge-timed-send", sv.chid);
    while (pid > 0 && !atomic_load(&held) && waited < ACK_TIMEOUT_MS) {
        ms_sleep(KS_POLL_MS);
        waited += KS_POLL_MS;
    }
    check(atomic_load(&held) != 0, "timed-send child reached the server", "no message arrived");
    if (pid > 0)
        (void) kill(pid, SIGKILL);
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "timed-send child reaped",
          "child was not reaped after SIGKILL");
    atomic_store(&sv.stop, 1);
    (void) pthread_join(th, NULL);
    (void) channel_destroy(sv.chid);
    gauge_check(fd, "SIGKILL in a timed MsgSend", "soul.timer.used", timer0);
    gauge_check(fd, "SIGKILL in a timed MsgSend", "soul.process.used", process0);
}

/* A process killed while a thread is blocked in MsgSend on a connection whose
 * descriptor a sibling already closed: the dying sender holds the last
 * reference. The soul must go back (the disconnect notice reaches the server,
 * which detaches its scoid) while the channel is still alive.
 *
 * Opt-in (`syscall_testing --orphan-send`), not in test_gauges(): on the dev tree
 * at CPUS=2 it hangs about 1 run in 3 after the gauge checks, with one hart in
 * rcu_sched_synchronize() under ksys_connect_detach() and the other inside
 * ksys_msg_receivev() (Q67). It passes with or without a change to
 * exit.c, so it does not pin the Q65 b drop; it is kept as a reproducer.
 */
void test_orphan_send_kill(void)
{
    int fd = open("/sys/kstats", O_RDONLY);

    struct srv sv = {.chid = channel_create(RTMUX_FLG_CHF_DISCONNECT)};
    struct srv drain = {.chid = sv.chid};
    atomic_int held = 0;
    pthread_t th, dth;
    long connect0, process0;
    pid_t pid;
    int status, waited = 0, nth = 0;

    if (fd < 0 || sv.chid < 0) {
        test_fail("orphan-send fixture", strerror(errno));
        if (fd >= 0)
            (void) close(fd);
        return;
    }
    sv.held = &held;
    connect0 = gauge_stable(fd, "soul.connect.used");
    process0 = gauge_stable(fd, "soul.process.used");
    nth += pthread_create(&th, NULL, srv_thread, &sv) == 0;
    nth += (pthread_create(&dth, NULL, srv_drain, &drain) == 0) << 1;
    pid = gauge_spawn("--gauge-orphan-send", sv.chid);
    while (pid > 0 && !atomic_load(&held) && waited < ACK_TIMEOUT_MS) {
        ms_sleep(KS_POLL_MS);
        waited += KS_POLL_MS;
    }
    check(atomic_load(&held) != 0, "orphan-send child reached the server", "no message arrived");
    ms_sleep(300); /* the child closes its descriptor 100 ms after sending */
    if (pid > 0)
        (void) kill(pid, SIGKILL);
    check(pid > 0 && waitpid(pid, &status, 0) == pid, "orphan-send child reaped",
          "child was not reaped after SIGKILL");
    gauge_check(fd, "SIGKILL with the last reference held by a blocked sender",
                "soul.connect.used", connect0);
    gauge_check(fd, "SIGKILL with the last reference held by a blocked sender",
                "soul.process.used", process0);
    atomic_store(&sv.stop, 1);
    atomic_store(&drain.stop, 1);
    if (nth & 1)
        (void) pthread_join(th, NULL);
    (void) channel_destroy(sv.chid);
    if (nth & 2)
        (void) pthread_join(dth, NULL);
    (void) close(fd);
}

/* The parent polls waitpid(WNOHANG) as fast as it can while the child sheds its
 * sleeping threads, so the status is usually collected after proc_exit posted
 * it but before the last thread is gone; a detached (NOZOMBIE) child is never
 * waited for at all. Neither may leave the process behind.
 */
static void test_reap_races(int fd)
{
    long process0 = gauge_stable(fd, "soul.process.used");
    int ok = 0;

    for (int i = 0; i < GAUGE_CHILDREN; i++) {
        pid_t pid = gauge_spawn("--gauge-linger", -1);
        int status;

        while (pid > 0) {
            pid_t r = waitpid(pid, &status, WNOHANG);

            if (r == pid) {
                ok += WIFEXITED(status);
                break;
            }
            if (r < 0)
                break;
        }
    }
    check(ok == GAUGE_CHILDREN, "reap-race children exit", "a child was lost");
    gauge_check(fd, "polling waitpid against a lingering last thread", "soul.process.used",
                process0);

    ok = 0;
    for (int i = 0; i < GAUGE_CHILDREN; i++)
        ok += reaped_clean(gauge_spawn("--gauge-detach-exit", -1));
    check(ok == GAUGE_CHILDREN, "detached children report at detach", "waitpid after detach");
    gauge_check(fd, "detached (NOZOMBIE) children", "soul.process.used", process0);
}

void test_gauges(void)
{
    int fd = open("/sys/kstats", O_RDONLY);

    if (fd < 0) {
        test_fail("gauges open /sys/kstats", strerror(errno));
        return;
    }
    test_pipe_gauge(fd);
    test_spawn_gauge(fd);
    test_reap_races(fd);
    test_timed_send_kill(fd);
    test_detach_race(fd);
    (void) close(fd);
}

/* ---- lap gauge comparison ------------------------------------------------ */

struct pool_gauge {
    char name[48];
    long base;
};

static struct pool_gauge lap_base[KS_POOLS];
static int lap_npools;

/* Every soul.<pool>.used value in a snapshot, in file order. */
static int pools_parse(const char *snap, struct pool_gauge *out)
{
    int n = 0;

    for (const char *p = snap; p && *p && n < KS_POOLS;
         p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        const char *eq = strchr(p, '=');
        size_t len = eq ? (size_t) (eq - p) : 0;

        if (len > 10 && len < sizeof out[0].name && !strncmp(p, "soul.", 5) &&
            !strncmp(p + len - 5, ".used", 5)) {
            memcpy(out[n].name, p, len);
            out[n].name[len] = '\0';
            out[n].base = strtol(eq + 1, NULL, 10);
            out[n].base = pool_adjust(snap, out[n].name, out[n].base);
            n++;
        }
    }
    return n;
}

/* Sample the live counts between laps. Lap 1 is a warm-up (lazily created
 * per-thread timers and pool threads reach their steady state there and in lap
 * 2), so nothing is sampled; the end of lap LAP_BASELINE records the baseline;
 * later laps report every pool above it, after up to KS_SETTLE_MS for
 * asynchronous teardown. Returns the number that grew.
 */
static int lap_check_fd(int lap, int fd)
{
    struct pool_gauge cur[KS_POOLS], prev[KS_POOLS];
    char snap[KS_BUF];
    int grown = 0, n = 0, prev_n = -1;

    if (lap < LAP_BASELINE)
        return 0;
    for (int waited = 0; waited <= KS_SETTLE_MS; waited += KS_POLL_MS) {
        if (kstats_read(fd, snap, sizeof snap) <= 0) {
            printf("lap=%d kstats: unreadable: %s\n", lap, strerror(errno));
            return 1;
        }
        n = pools_parse(snap, cur);
        if (!lap_npools) { /* baseline: first sample that matches its predecessor */
            int same = prev_n == n;

            for (int i = 0; same && i < n; i++)
                same = cur[i].base == prev[i].base;
            if (same || waited >= KS_SETTLE_MS) {
                memcpy(lap_base, cur, sizeof cur);
                lap_npools = n;
                return 0;
            }
            memcpy(prev, cur, sizeof cur);
            prev_n = n;
            ms_sleep(KS_POLL_MS);
            continue;
        }
        grown = 0;
        for (int i = 0; i < n && i < lap_npools; i++)
            grown += cur[i].base > lap_base[i].base;
        if (!grown || n != lap_npools)
            break;
        ms_sleep(KS_POLL_MS);
    }
    if (n != lap_npools) {
        printf("lap=%d kstats: pool set changed (%d -> %d)\n", lap, lap_npools, n);
        return 1;
    }
    for (int i = 0; i < n; i++)
        if (cur[i].base > lap_base[i].base)
            printf("lap=%d gauge %s base=%ld now=%ld\n", lap, cur[i].name,
                   lap_base[i].base, cur[i].base);
    return grown;
}

/* A fresh descriptor per call: a suite may close descriptors it did not open,
 * and the open/close pair leaves the connection count where it was.
 */
int leak_lap_check(int lap)
{
    int fd = open("/sys/kstats", O_RDONLY), grown;

    if (fd < 0) {
        printf("lap=%d kstats: open failed: %s\n", lap, strerror(errno));
        return 1;
    }
    grown = lap_check_fd(lap, fd);
    (void) close(fd);
    return grown;
}

/* Name every live process ("pid:name"), so a suite that leaves one behind says
 * which. Parameters:
 *   lap, suite : for the line prefix
 */
static void print_procs(int lap, const char *suite)
{
    DIR *d = opendir("/proc");
    struct dirent *de;

    if (!d)
        return;
    printf("lap=%d suite=%s procs:", lap, suite);
    while ((de = readdir(d))) {
        char path[64], name[48];
        int fd;
        ssize_t len;

        if (de->d_name[0] < '0' || de->d_name[0] > '9')
            continue;
        snprintf(path, sizeof path, "/proc/%s/name", de->d_name);
        name[0] = '\0';
        if ((fd = open(path, O_RDONLY)) >= 0) {
            if ((len = read(fd, name, sizeof name - 1)) > 0)
                name[len - (name[len - 1] == '\n')] = '\0';
            (void) close(fd);
        }
        printf(" %s:%s", de->d_name, name);
    }
    printf("\n");
    (void) closedir(d);
}

/* Per-suite attribution for --laps: after each suite, print every pool whose
 * settled live count differs from the count after the previous suite ("lap=<n>
 * suite=<name> gauge <pool> <before> -> <after>"). Settled means two samples
 * 50 ms apart agree. Lap 1 only records, so a pool that changes only in lap 1
 * (lazy pool growth) stays silent.
 */
void leak_suite_delta(int lap, const char *suite)
{
    static struct pool_gauge before[KS_POOLS];
    static int before_n;
    static long live_before;
    long live;
    struct pool_gauge cur[KS_POOLS], again[KS_POOLS];
    char snap[KS_BUF];
    int fd = open("/sys/kstats", O_RDONLY), n = 0;

    if (fd < 0)
        return;
    for (int tries = 0; tries < 20; tries++) {
        if (kstats_read(fd, snap, sizeof snap) <= 0)
            break;
        n = pools_parse(snap, cur);
        ms_sleep(50);
        if (kstats_read(fd, snap, sizeof snap) <= 0)
            break;
        if (pools_parse(snap, again) == n && !memcmp(cur, again, sizeof cur[0] * (size_t) n))
            break;
    }
    (void) close(fd);
    /* Physical memory is not a soul pool; report it so a lap that runs the
     * allocator dry shows which suite took the memory.
     */
    printf("lap=%d suite=%s pa.free_bytes=%ld roottask.heap_used=%ld\n", lap, suite,
           kstats_get(snap, "pa.free_bytes"), kstats_get(snap, "roottask.heap_used"));
    live = kstats_get(snap, "thread.live");
    if (lap > 1 && live != live_before)
        printf("lap=%d suite=%s gauge thread.live %ld -> %ld\n", lap, suite, live_before, live);
    live_before = live;
    for (int i = 0; lap > 1 && i < n && i < before_n; i++)
        if (cur[i].base != before[i].base) {
            printf("lap=%d suite=%s gauge %s %ld -> %ld\n", lap, suite, cur[i].name,
                   before[i].base, cur[i].base);
            if (!strcmp(cur[i].name, "soul.process.used") && cur[i].base > before[i].base)
                print_procs(lap, suite);
        }
    memcpy(before, cur, sizeof cur);
    before_n = n;
}
