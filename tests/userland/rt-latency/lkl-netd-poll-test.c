/*
 * lkl-netd-poll-test - poll() on lkl-netd data sockets.
 *
 * The peer is the host helper that qemu-regress.sh starts for this mode, at
 * 10.0.2.2: one port per behaviour, so a case needs no message of its own.
 *
 *   8004 idle    accepts and says nothing
 *   8005 data    sends "hello" one second after accept
 *   8006 reset   resets the connection one second after accept
 *   8007 finish  closes the connection (FIN) one second after accept
 *   8008 late    sends "hello" two seconds after accept
 *
 * Cases: a socket that is idle, becomes readable, is writable, is reset and
 * is closed by its peer; a hangup-only poll (events == 0), which has to end
 * when the peer goes and not after its whole timeout; a poll that runs while
 * a ping holds the admission lock; an arm on a socket the watcher already
 * polls, which must not kick it again; and a slot that is reconnected 1000
 * times under a poller, which must never see readiness that is not there.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/poll.h>

#include "../outcome.h"

#define PORT_IDLE 8004
#define PORT_DATA 8005
#define PORT_RESET 8006
#define PORT_FIN 8007
#define PORT_LATE 8008
#define PING "/net/ping"
#define STATS "/net/stats"
#define STALE_CYCLES 1000
#define KICK_ARMS 40
#define KICK_ALLOWED 4

static struct test_counts counts;

static long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static void nap_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (ms % 1000) * 1000000};

    nanosleep(&ts, NULL);
}

static void check(const char *name, int ok, const char *detail)
{
    if (ok)
        tc_ok(&counts, name);
    else
        tc_fail(&counts, name, detail);
}

struct sock {
    int ctl;
    int data;
    char id[16];
};

static int sock_connect(struct sock *s, int port)
{
    char cmd[64];

    snprintf(cmd, sizeof cmd, "connect 10.0.2.2 %d\n", port);
    return write(s->ctl, cmd, strlen(cmd)) < 0 ? -1 : 0;
}

/* A new slot, connected to the peer's port. */
static int sock_open(struct sock *s, int port)
{
    char path[48];
    ssize_t n;
    int fd;

    s->ctl = s->data = -1;
    fd = open("/net/tcp/clone", O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, s->id, sizeof s->id - 1);
    close(fd);
    if (n <= 0)
        return -1;
    s->id[n] = '\0';
    while (n > 0 && (s->id[n - 1] == '\n' || s->id[n - 1] == '\r'))
        s->id[--n] = '\0';
    snprintf(path, sizeof path, "/net/tcp/%s/ctl", s->id);
    s->ctl = open(path, O_WRONLY);
    if (s->ctl < 0 || sock_connect(s, port) != 0)
        goto fail;
    snprintf(path, sizeof path, "/net/tcp/%s/data", s->id);
    s->data = open(path, O_RDWR);
    if (s->data >= 0)
        return 0;
fail:
    if (s->ctl >= 0)
        close(s->ctl);
    s->ctl = -1;
    return -1;
}

static void sock_close(struct sock *s)
{
    if (s->data >= 0)
        close(s->data);
    if (s->ctl >= 0)
        close(s->ctl);
    s->data = s->ctl = -1;
}

/* Readable, with no error or hangup beside it. */
static int only_readable(int rev)
{
    return rev > 0 && (rev & POLLIN) && !(rev & (POLLERR | POLLHUP));
}

/* poll one descriptor; *ms gets the time it took. Returns revents, or -1. */
static int poll1(int fd, short events, int timeout_ms, long *ms)
{
    struct pollfd pfd = {.fd = fd, .events = events};
    long t0 = now_ms();
    int rc = poll(&pfd, 1, timeout_ms);

    if (ms)
        *ms = now_ms() - t0;
    return rc < 0 ? -1 : (rc == 0 ? 0 : pfd.revents);
}

/* The value of a "name N" line of /net/stats, or -1. */
static long stat_value(const char *name)
{
    char buf[1536];
    ssize_t n;
    char *p;
    int fd = open(STATS, O_RDONLY);

    if (fd < 0)
        return -1;
    n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    p = strstr(buf, name);
    if (!p || p[strlen(name)] != ' ')
        return -1;
    return strtol(p + strlen(name) + 1, NULL, 10);
}

static void case_idle(void)
{
    struct sock s;
    long ms;
    int rev;

    if (sock_open(&s, PORT_IDLE) != 0) {
        tc_fail(&counts, "idle-connect", strerror(errno));
        return;
    }
    rev = poll1(s.data, POLLIN, 300, &ms);
    check("idle-pollin-quiet", rev == 0 && ms >= 250,
          "an idle socket polled in");
    rev = poll1(s.data, POLLOUT, 300, &ms);
    check("idle-pollout-ready", rev == POLLOUT && ms < 250,
          "a connected socket was not writable at once");
    rev = poll1(s.data, POLLIN | POLLOUT, 300, &ms);
    check("idle-both-out-only", rev == POLLOUT,
          "POLLIN with POLLOUT on an idle socket");
    rev = poll1(s.data, 0, 300, &ms);
    check("idle-hup-only-quiet", rev == 0 && ms >= 250,
          "a hangup-only poll ended on a live socket");
    check("write-then-writable",
          write(s.data, "x", 1) == 1 &&
              poll1(s.data, POLLOUT, 300, NULL) == POLLOUT,
          "not writable after a write");
    sock_close(&s);
}

static void case_data(void)
{
    struct sock s;
    char buf[16];
    long ms;
    int rev;

    if (sock_open(&s, PORT_DATA) != 0) {
        tc_fail(&counts, "data-connect", strerror(errno));
        return;
    }
    rev = poll1(s.data, POLLIN, 8000, &ms);
    check("data-wakes-pollin", only_readable(rev) && ms < 6000,
          "poll did not report the peer's data");
    check("data-read",
          read(s.data, buf, sizeof buf) == 5 && !memcmp(buf, "hello", 5),
          "read did not return the data");
    check("data-drained", poll1(s.data, POLLIN, 200, NULL) == 0,
          "still readable after the read");
    sock_close(&s);
}

static void case_reset(void)
{
    struct sock s;
    long ms;
    int rev;

    if (sock_open(&s, PORT_RESET) != 0) {
        tc_fail(&counts, "reset-connect", strerror(errno));
        return;
    }

    /* No events: only the hangup can end this, and it has to, promptly. */
    rev = poll1(s.data, 0, 8000, &ms);
    check("reset-hup-only-wakes", rev > 0 && (rev & POLLHUP) && ms < 6000,
          "a hangup-only poll slept through the peer's reset");
    rev = poll1(s.data, POLLIN, 1000, NULL);
    check("reset-pollin-reports", rev > 0 && (rev & POLLHUP),
          "the hangup was not reported again");
    sock_close(&s);
}

static void case_fin(void)
{
    struct sock s;
    char buf[8];
    long ms;
    int rev;

    if (sock_open(&s, PORT_FIN) != 0) {
        tc_fail(&counts, "fin-connect", strerror(errno));
        return;
    }
    rev = poll1(s.data, POLLIN, 8000, &ms);
    check("fin-wakes-pollin", rev > 0 && (rev & POLLIN) && ms < 6000,
          "poll did not report the peer's close");
    check("fin-read-eof", read(s.data, buf, sizeof buf) == 0, "no end of file");
    sock_close(&s);
}

/* The ping holds the admission lock for its whole timeout. */
static void *ping_thread(void *arg)
{
    int *state = arg;
    int fd = open(PING, O_WRONLY);

    if (fd < 0) {
        __atomic_store_n(state, -1, __ATOMIC_RELEASE);
        return NULL;
    }
    __atomic_store_n(state, 1, __ATOMIC_RELEASE);
    (void) write(fd, "10.0.2.254 3\n", 13);
    close(fd);
    __atomic_store_n(state, 2, __ATOMIC_RELEASE);
    return NULL;
}

static void case_contention(void)
{
    int state = 0;
    pthread_t t;
    struct sock s;
    char buf[8];
    int locked = 0;
    long ms;
    int rev;

    if (sock_open(&s, PORT_LATE) != 0) {
        tc_fail(&counts, "contention-connect", strerror(errno));
        return;
    }
    if (pthread_create(&t, NULL, ping_thread, (void *) &state) != 0) {
        tc_fail(&counts, "contention-thread", strerror(errno));
        sock_close(&s);
        return;
    }
    for (int i = 0; i < 50 && __atomic_load_n(&state, __ATOMIC_ACQUIRE) == 0;
         i++)
        nap_ms(20);
    for (int i = 0; i < 50 && !locked; i++) {
        int fd = open(STATS, O_RDONLY);

        if (fd >= 0) {
            close(fd);
            nap_ms(20);
        } else {
            locked = errno == EAGAIN;
            if (!locked)
                break;
        }
    }
    if (!locked) {
        tc_skip(&counts, "contention",
                "the admission lock was never seen taken");
        pthread_join(t, NULL);
        sock_close(&s);
        return;
    }

    /* The data comes two seconds after the connect, while the ping runs: the
     * arm, the wake and the disarm pass all go through without the lock.
     */
    rev = poll1(s.data, POLLIN, 8000, &ms);
    check("contention-poll-wakes", only_readable(rev) && ms < 7000,
          "poll under the admission lock did not report the data");
    pthread_join(t, NULL);
    check("contention-read-after", read(s.data, buf, sizeof buf) == 5,
          "read after the lock was freed");
    sock_close(&s);
}

struct pollthr {
    int fd;
    int stop;
    int stale; /* readable with nothing to read */
    int rounds;
};

static void *poller(void *arg)
{
    struct pollthr *p = arg;

    while (!__atomic_load_n(&p->stop, __ATOMIC_ACQUIRE)) {
        int rev = poll1(p->fd, POLLIN, 30, NULL);

        p->rounds++;
        if (only_readable(rev))
            p->stale++;
        if (rev & (POLLERR | POLLHUP))
            nap_ms(1); /* the slot is between sockets */
    }
    return NULL;
}

/* One long poll, so that its arm stays in place while the others come and go.
 */
static void *long_poll(void *arg)
{
    const int *fd = arg;

    (void) poll1(*fd, POLLIN, 3000, NULL);
    return NULL;
}

static void case_kick(void)
{
    pthread_t t;
    struct sock s;
    char detail[96];
    long before, after;

    if (sock_open(&s, PORT_IDLE) != 0) {
        tc_fail(&counts, "kick-connect", strerror(errno));
        return;
    }
    if (pthread_create(&t, NULL, long_poll, &s.data) != 0) {
        tc_fail(&counts, "kick-thread", strerror(errno));
        sock_close(&s);
        return;
    }
    nap_ms(300); /* it is armed, and the socket is in the watcher's set */
    before = stat_value("poll_kicks");
    for (int i = 0; i < KICK_ARMS; i++)
        (void) poll1(s.data, POLLIN, 5, NULL);
    after = stat_value("poll_kicks");
    pthread_join(t, NULL);
    snprintf(detail, sizeof detail,
             "%d arms on a polled socket kicked the watcher %ld times",
             KICK_ARMS, after - before);
    if (before < 0 || after < 0)
        tc_fail(&counts, "kick-stats", "no poll_kicks in /net/stats");
    else
        check("kick-only-new-sockets", after - before <= KICK_ALLOWED, detail);
    sock_close(&s);
}

static void case_stale(void)
{
    struct pollthr p = {0};
    pthread_t t;
    struct sock s;
    int done = 0;

    if (sock_open(&s, PORT_IDLE) != 0) {
        tc_fail(&counts, "stale-connect", strerror(errno));
        return;
    }
    p.fd = s.data;
    if (pthread_create(&t, NULL, poller, &p) != 0) {
        tc_fail(&counts, "stale-thread", strerror(errno));
        sock_close(&s);
        return;
    }

    /* Each connect on the slot closes its socket and opens another, which LKL
     * will usually give the same number.
     */
    for (; done < STALE_CYCLES; done++)
        if (sock_connect(&s, PORT_IDLE) != 0)
            break;
    nap_ms(100);
    __atomic_store_n(&p.stop, 1, __ATOMIC_RELEASE);
    pthread_join(t, NULL);
    check("stale-cycles-done", done == STALE_CYCLES, "a reconnect failed");
    check("stale-poller-ran", p.rounds >= 5,
          "the poller made too few polls to race the reconnects");
    check("stale-no-false-ready", p.stale == 0,
          "a reconnected slot reported readiness that was not there");
    check("stale-still-polls", poll1(s.data, POLLOUT, 300, NULL) == POLLOUT,
          "the reconnected socket is not writable");
    sock_close(&s);
}

int main(void)
{
    case_idle();
    case_data();
    case_reset();
    case_fin();
    case_contention();
    case_kick();
    case_stale();
    tc_summary(&counts);
    if (tc_status(&counts, 1)) {
        printf("lkl-netd-poll-test: FAIL\n");
        return 1;
    }
    printf("lkl-netd-poll-test: PASS\n");
    return 0;
}
