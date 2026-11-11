/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * notifications.c — async notification syscall regressions.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <time.h>
#include <sys/wait.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sched.h>
#include <stdalign.h>
#include <unistd.h>
#include <sys/rtmux-core.h>

#include "test.h"

_Static_assert(sizeof(struct _notification) == 24,
               "struct _notification ABI size changed");
_Static_assert(alignof(struct _notification) == 8,
               "struct _notification ABI alignment changed");
_Static_assert(offsetof(struct _notification, type) == 0,
               "struct _notification type offset changed");
_Static_assert(offsetof(struct _notification, subtype) == 2,
               "struct _notification subtype offset changed");
_Static_assert(offsetof(struct _notification, code) == 4,
               "struct _notification code offset changed");
_Static_assert(offsetof(struct _notification, value) == 8,
               "struct _notification value offset changed");
_Static_assert(offsetof(struct _notification, scoid) == 16,
               "struct _notification scoid offset changed");

static int recv_notification(int chid, int code, int value, const char *name)
{
    struct _notification notification;
    char detail[128];
    int rc;

    memset(&notification, 0xa5, sizeof notification);
    errno = 0;
    rc = msg_receive_notification(chid, &notification, sizeof notification,
                                  NULL);
    snprintf(detail, sizeof detail, "rc=%d errno=%d code=%d value=%d", rc,
             errno, notification.code, notification.value.sival_int);
    check(rc == 0 && notification.type == _NOTIFICATION_TYPE &&
              notification.subtype == _NOTIFICATION_SUBTYPE &&
              notification.code == code &&
              notification.value.sival_int == value && notification.scoid != 0,
          name, detail);

    return rc;
}

struct notification_wait_sender {
    int coid;
    atomic_int started;
    int result;
};

static void *notification_wait_sender(void *arg)
{
    struct notification_wait_sender *sender = arg;
    int message = 42;

    atomic_store(&sender->started, 1);
    sender->result = msg_send(sender->coid, &message, sizeof message, NULL, 0);
    return NULL;
}

/* An ordinary queued sender must not make a notification-only receive spin;
 * the receive must time out and leave that sender for a normal receive. */
static void test_notification_with_queued_sender(void)
{
    struct notification_wait_sender sender = {.coid = -1, .result = -1};
    struct _notification notification;
    struct timespec settle = {.tv_nsec = 100000000L};
    uint64_t timeout_ns = 100000000ULL;
    pthread_t thread;
    int chid, rcvid, message = 0, rc, saved_errno, create_error;

    chid = channel_create(0);
    if (chid < 0) {
        test_fail("notification queued sender channel", strerror(errno));
        return;
    }
    sender.coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (sender.coid < 0) {
        test_fail("notification queued sender connect", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }
    create_error = pthread_create(&thread, NULL, notification_wait_sender, &sender);
    if (create_error != 0) {
        test_fail("notification queued sender thread", strerror(create_error));
        (void) __connect_detach((uint64_t) sender.coid);
        (void) channel_destroy(chid);
        return;
    }
    while (!atomic_load(&sender.started))
        sched_yield();
    /* There is no queue-state query here; allow the sender time to enter the
     * kernel. The pre-fix run must hang to confirm this setup hit the race. */
    (void) nanosleep(&settle, NULL);
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);
    errno = 0;
    rc = msg_receive_notification(chid, &notification, sizeof notification, NULL);
    saved_errno = errno;
    check(rc == -1 && saved_errno == ETIMEDOUT,
          "notification receive ignores queued sender", "expected receive timeout");
    check(msg_send_notification(sender.coid, 5, 77, 1) == EOK,
          "notification queued behind sender setup", "notification send failed");
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);
    (void) recv_notification(chid, 77, 1, "notification received with sender queued");
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &timeout_ns, NULL);
    rcvid = msg_receive(chid, &message, sizeof message, NULL);
    check(rcvid > 0 && message == 42, "queued sender remains receivable",
          "expected the original message");
    if (rcvid > 0)
        (void) msg_reply(rcvid, EOK, NULL, 0);
    else {
        /* Unblock the sender before joining it on a failed receive. */
        (void) channel_destroy(chid);
        chid = -1;
    }
    (void) pthread_join(thread, NULL);
    check(sender.result == EOK, "queued sender receives reply", "send failed");
    (void) __connect_detach((uint64_t) sender.coid);
    if (chid >= 0)
        (void) channel_destroy(chid);
}

/* Per-connection send-right revocation: revoking a live connection's send right
 * blocks subsequent notification sends without tearing the connection down.
 */
static void test_connection_send_revoke(void)
{
    int chid, coid, rc;

    chid = channel_create(0);
    if (chid == -1) {
        test_fail("send-revoke channel_create", strerror(errno));
        return;
    }
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        test_fail("send-revoke connect_attach", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }

    /* A notification send succeeds while the send right is held. It also queues
     * one notification that the still-live check below drains.
     */
    check(msg_send_notification(coid, 5, 10, 100) == EOK,
          "send-revoke send before revoke",
          "notification send must succeed while the send right is held");

    rc = connect_flags(0, (uint64_t) coid, RTMUX_FLG_COF_REVOKE_SEND,
                       RTMUX_FLG_COF_REVOKE_SEND);
    check(rc != -1, "send-revoke connect_flags revoke",
          "connect_flags RTMUX_FLG_COF_REVOKE_SEND must succeed");

    /* A send on the now-revoked connection is refused. */
    {
        char detail[64];
        int sr = msg_send_notification(coid, 5, 10, 100);
        snprintf(detail, sizeof detail, "send returned %d, expected refusal", sr);
        check(sr != EOK, "send-revoke blocks notification", detail);
    }

    /* The connection is still live: the notification queued before the revoke
     * is still receivable, so the revoke stripped the send right without
     * tearing the connection down.
     */
    (void) recv_notification(chid, 10, 100, "send-revoke connection still live");

    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
}

/* disconnect_under_alloc_failure: a client's disconnect notice must reach the
 * server even when the server's ordinary notification allocation is failing.
 *
 * The server is this process: it owns a CHF_DISCONNECT channel, a child makes
 * DISC_CONNS connections to it, and the child is SIGKILLed with a one-shot
 * allocation failure armed against this pid. Each lost notice would show as a
 * missing queued notification, so the check never blocks on a receive. Needs
 * the debug fault-injection call (/bin/inject, installed only on a
 * CONFIG_DEBUG_INJECT kernel); without it the case is skipped.
 */
#define DISC_CONNS 16
#define DISC_ROUNDS 100
#define DISC_SETTLE_NS 20000000L
#define DISC_WAIT_POLLS 200 /* x DISC_SETTLE_NS = 4 s */
#define DISC_PID_NONE "2147483647"

static int disc_fd = -1;

static long disc_stat(const char *key)
{
    char snap[4096];
    size_t klen = strlen(key);
    ssize_t n, total = 0;

    if (lseek(disc_fd, 0, SEEK_SET) != 0)
        return -1;
    while (total < (ssize_t) sizeof snap - 1 &&
           (n = read(disc_fd, snap + total, sizeof snap - 1 - (size_t) total)) > 0)
        total += n;
    snap[total < 0 ? 0 : total] = '\0';
    for (const char *p = snap; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, key, klen) && p[klen] == '=')
            return strtol(p + klen + 1, NULL, 10);
    return -1;
}

static void disc_nap(void)
{
    struct timespec ts = {.tv_nsec = DISC_SETTLE_NS};

    (void) nanosleep(&ts, NULL);
}

/* Poll until "key" reaches "want" (want >= 0), or holds one value over two
 * consecutive samples (want < 0). Returns the last value read.
 */
static long disc_wait(const char *key, long want)
{
    long v = disc_stat(key), prev = -2;

    for (int i = 0; i < DISC_WAIT_POLLS; i++) {
        if (want >= 0 ? v >= want : v == prev)
            break;
        prev = v;
        disc_nap();
        v = disc_stat(key);
    }
    return v;
}

/* Run /bin/inject to completion: its exit status, or -1 if it did not run. */
static int disc_inject(const char *a1, const char *a2, const char *a3)
{
    char *argv[] = {"inject", (char *) a1, (char *) a2, (char *) a3, NULL};
    pid_t pid;
    int st;

    if (posix_spawn(&pid, "/bin/inject", NULL, NULL, argv, NULL) != 0 ||
        waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        return -1;
    return WEXITSTATUS(st);
}

/* Child: connect to the parent's channel DISC_CONNS times, then park. */
int disconnect_child(const char *pid_arg, const char *chid_arg)
{
    int pid = atoi(pid_arg), chid = atoi(chid_arg);

    for (int i = 0; i < DISC_CONNS; i++)
        if (connect_attach(0, pid, chid, 0, RTMUX_FLG_COF_NOSHARE) == -1)
            _exit(2);
    for (;;)
        (void) sleep(1000);
}

static void disc_kill(pid_t pid)
{
    int st;

    (void) kill(pid, SIGKILL);
    (void) waitpid(pid, &st, 0);
}

/* One round; returns NULL on success, else a static failure description. */
static const char *disc_round(int chid, const char *pidarg, char *detail,
                              size_t dsz)
{
    char chidarg[16];
    char *cargv[] = {"syscall_testing", "--disconnect-child", (char *) pidarg,
                     chidarg, NULL};
    long conn0 = disc_wait("soul.connect.used", -1);
    long res0 = disc_stat("soul.notification.reserved");
    long nused0 = disc_stat("soul.notification.used");
    long conn1, res1, nused1;
    int got = 0;
    pid_t pid;

    snprintf(chidarg, sizeof chidarg, "%d", chid);
    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, cargv, NULL) != 0)
        return "spawn failed";
    (void) disc_wait("soul.connect.used", conn0 + DISC_CONNS);
    conn1 = disc_wait("soul.connect.used", -1); /* the child is parked */
    res1 = disc_stat("soul.notification.reserved");
    if (res1 < res0 + DISC_CONNS) {
        snprintf(detail, dsz, "reserved %ld -> %ld, connect.used %ld -> %ld",
                 res0, res1, conn0, conn1);
        disc_kill(pid);
        return "attach did not reserve a notice per connection";
    }
    if (disc_inject("fail-alloc", "notification", pidarg) != 0) {
        disc_kill(pid);
        return "inject refused to arm";
    }

    disc_kill(pid);
    (void) disc_wait("soul.notification.used", nused0 + DISC_CONNS);
    nused1 = disc_wait("soul.notification.used", -1);
    if (nused1 != nused0 + DISC_CONNS) {
        snprintf(detail, dsz, "%ld of %d notices queued", nused1 - nused0,
                 DISC_CONNS);
        return "disconnect notice lost";
    }

    /* All of them should be queued; the timeout turns a notice that is queued
     * but unreceivable into a failure instead of a hang.
     */
    for (int i = 0; i < DISC_CONNS; i++) {
        struct _notification n;
        uint64_t timeout_ns = 500000000ULL;

        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL,
                               &timeout_ns, NULL);
        if (msg_receive_notification(chid, &n, sizeof n, NULL) != 0)
            break;
        if (n.code == _NOTIFICATION_CODE_DISCONNECT) {
            got++;
            (void) __connect_detach((uint64_t) n.scoid);
        }
    }
    conn1 = disc_wait("soul.connect.used", conn0);
    res1 = disc_stat("soul.notification.reserved");
    nused1 = disc_wait("soul.notification.used", -1);
    if (got != DISC_CONNS || conn1 != conn0 || res1 != res0 || nused1 != nused0) {
        snprintf(detail, dsz,
                 "received %d, connect.used %+ld reserved %+ld notification.used %+ld",
                 got, conn1 - conn0, res1 - res0, nused1 - nused0);
        return "gauges did not return to baseline";
    }
    return NULL;
}

/* Returns 0 when it ran (the verdict goes through check), or DISCONNECT_SKIP
 * when fault injection is not available.
 */
int disconnect_under_alloc_failure(void)
{
    char pidarg[16], detail[160] = "";
    const char *why = NULL;
    int chid, round;

    if (disc_inject("alloc-status", NULL, NULL) != 0)
        return DISCONNECT_SKIP;
    disc_fd = open("/sys/kstats", O_RDONLY);
    chid = channel_create(RTMUX_FLG_CHF_DISCONNECT);
    if (disc_fd < 0 || chid == -1) {
        test_fail("disconnect_under_alloc_failure fixture", strerror(errno));
        if (disc_fd >= 0)
            (void) close(disc_fd);
        if (chid != -1)
            (void) channel_destroy(chid);
        return 0;
    }
    snprintf(pidarg, sizeof pidarg, "%d", (int) getpid());

    for (round = 0; round < DISC_ROUNDS && !why; round++)
        why = disc_round(chid, pidarg, detail, sizeof detail);
    if (why) {
        char msg[256];

        snprintf(msg, sizeof msg, "round %d: %s (%s)", round - 1, why, detail);
        test_fail("disconnect_under_alloc_failure", msg);
    } else {
        test_ok("disconnect_under_alloc_failure no notice lost in 100 rounds");
    }

    /* Leave no failure armed against this process. */
    (void) disc_inject("fail-alloc", "notification", DISC_PID_NONE);
    (void) channel_destroy(chid);
    (void) close(disc_fd);
    return 0;
}

void test_notifications(void)
{
    struct sched_param before;
    struct sched_param sp;
    int restore_sched = 0;
    int check_boost = 0;
    int boosted = 0;
    int chid;
    int coid;

    memset(&before, 0, sizeof before);
    if (__sched_get(0, 0, &before) == EOK) {
        sp = before;
        sp.sched_priority = 4;
        if (__sched_set(0, 0, SCHED_FIFO, &sp) == EOK) {
            restore_sched = 1;
            check_boost = 1;
        }
    }

    chid = channel_create(0);
    if (chid == -1) {
        test_fail("notification channel_create", strerror(errno));
        return;
    }
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        test_fail("notification connect_attach", strerror(errno));
        (void) channel_destroy(chid);
        return;
    }

    check(msg_send_notification(coid, 5, 10, 100) == EOK &&
              msg_send_notification(coid, 5, 10, 100) == EOK &&
              msg_send_notification(coid, 12, 20, 200) == EOK &&
              msg_send_notification(coid, 7 | RTMUX_NOTIFICATION_REPLACE, 30,
                                    300) == EOK &&
              msg_send_notification(coid, 7 | RTMUX_NOTIFICATION_REPLACE, 30,
                                    301) == EOK,
          "notification send queue setup",
          "expected all queued notification sends to succeed");

    if (recv_notification(chid, 20, 200,
                          "notification priority delivery order") == 0 &&
        check_boost && __sched_get(0, 0, &sp) == EOK) {
        boosted = sp.sched_curpriority >= 12;
    }
    if (check_boost)
        check(boosted, "notification receiver priority boost",
              "expected receive to boost current priority to notification");

    (void) recv_notification(chid, 30, 301, "notification replace value");
    (void) recv_notification(chid, 10, 100, "notification coalesced first");
    (void) recv_notification(chid, 10, 100, "notification coalesced second");

    if (restore_sched)
        (void) __sched_set(0, 0, SCHED_FIFO, &before);
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);

    test_connection_send_revoke();
    test_notification_with_queued_sender();
    if (disconnect_under_alloc_failure() == DISCONNECT_SKIP)
        printf("  SKIP  disconnect_under_alloc_failure (fault injection "
               "unavailable)\n");
}
