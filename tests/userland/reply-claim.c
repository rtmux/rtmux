/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * reply-claim — a reply's claim on its client, at the two points where it
 * used to go wrong.
 *
 * - A channel destroy on another hart detaches the connection between the
 *   reply's rcvid lookup and its read of the connection's channel. The reply
 *   must fail with ESRCH; it used to lock through the cleared pointer.
 * - A reply that removes a queued unblock notification and is then preempted
 *   restarts. The restart must give the claim up, or the restarted call finds
 *   its own stale claim and reports the client gone.
 * - A send finds only a receiver that waits for notifications, which it may
 *   not claim, and that receiver is woken before the send looks at it again.
 *   The send must queue; it used to take the thread for a receiver it had
 *   claimed and deliver to it, and make it ready, a second time.
 * - A client's REPLY timeout finds it unclaimed and goes on to queue its
 *   unblock notification while a reply claims it and looks for one. The
 *   client must leave with no notification marked queued: a stale mark kept
 *   the kernel from asking for the unblock of the client's next request, which
 *   a server that parks requests then held for good.
 *
 * These windows are a few instructions wide, so the cases hold them open with
 * the debug fault-injection call (/bin/inject, CONFIG_DEBUG_INJECT kernels
 * only) and are skipped without it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/states.h>

#include "test.h"

#define WAIT_MS 5000    /* the longest any wait in a case may take */
#define STALL_MS "3000" /* the stall's own limit, if no destroy ends it */
#define UNBLOCK_MS 20
#define REPLY_STATUS 77
#define HOLD_MS "3000" /* reply_unblock_race's hold on each side */
#define SPIN_MS 500    /* and when its reply goes out */

struct call {
    int id;              /* the coid to send on, or the rcvid to reply to */
    int status;          /* the reply's status */
    unsigned pin_mask;   /* the replier's harts; 0 for wherever it is */
    unsigned timeout_ms; /* the send's REPLY timeout; 0 for none */
    atomic_int done;
    int rc, err;
};

static void *client_main(void *raw)
{
    struct call *c = raw;
    char req = 0, rep[REPLY_STATUS]; /* room for the status not to be capped */

    if (c->timeout_ms) { /* makes the kernel ask the server for an unblock */
        uint64_t ns = c->timeout_ms * 1000000ULL;

        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_REPLY, NULL, &ns,
                               NULL);
    }
    c->rc = msg_send(c->id, &req, 1, rep, sizeof rep);
    c->err = errno;
    atomic_store(&c->done, 1);
    return NULL;
}

static void *replier_main(void *raw)
{
    struct call *c = raw;
    char rep = 0;

    if (c->pin_mask)
        (void) test_pin_mask(c->pin_mask);
    c->rc = msg_reply(c->id, c->status, &rep, 1);
    c->err = errno;
    atomic_store(&c->done, 1);
    return NULL;
}

static bool wait_done(struct call *c)
{
    return test_wait_flag(&c->done, 1, WAIT_MS);
}

static void reply_detached_connection(const char *pid)
{
    static const char name[] = "reply_detached_connection";
    /* The replier runs off the destroyer's hart: the stall keeps its own. */
    struct call client = {0}, replier = {.pin_mask = 2};
    struct stall_watch watch = {.pid = -1};
    pthread_t tc, tr;
    int chid, coid;
    char buf[1], detail[96];
    bool entered;

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        test_skip(name, "needs two harts");
        return;
    }
    if (test_pin_mask(1) != 0 || !test_chan_open(0, &chid, &coid)) {
        test_fail(name, "pin or channel setup failed");
        return;
    }
    client.id = coid;
    if (pthread_create(&tc, NULL, client_main, &client) != 0) {
        test_fail(name, "client thread");
        return;
    }
    replier.id = msg_receive(chid, buf, sizeof buf, NULL);
    if (replier.id == -1 ||
        inject_run("stall-reply-claim", pid, STALL_MS) != 0 ||
        stall_watch_start(&watch, "reply-claim") != 0 ||
        pthread_create(&tr, NULL, replier_main, &replier) != 0) {
        test_fail(name, "receive, arm or replier thread failed");
        return;
    }

    /* The replier is now inside its kcall, past the lookup. The destroy
     * drains the client, unpublishes the connection and clears its channel,
     * which is what ends the stall.
     */
    entered = stall_watch_entered(&watch);
    (void) channel_destroy(chid);
    stall_watch_finish(&watch);
    check(entered, "reply_detached_connection stall taken",
          "the reply never reached the injected window");
    if (!wait_done(&replier) || !wait_done(&client)) {
        test_fail(name, "reply or send did not return");
        return;
    }
    (void) pthread_join(tr, NULL);
    (void) pthread_join(tc, NULL);
    check(inject_run("site-wait", "reply-detached", NULL) == 0,
          "reply_detached_connection detach seen in the window",
          "the stall timed out before the destroy cleared the channel");
    snprintf(detail, sizeof detail, "reply rc=%d errno=%d, send rc=%d errno=%d",
             replier.rc, replier.err, client.rc, client.err);
    check(replier.rc == -1 && replier.err == ESRCH && client.rc == -1, name,
          detail);
    (void) __connect_detach((uint64_t) coid);
    (void) test_pin_mask(~0U);
}

static void reply_unblock_restart(const char *pid)
{
    static const char name[] = "reply_unblock_restart";
    struct call client = {.timeout_ms = UNBLOCK_MS}, replier = {0};
    pthread_t tc, tr;
    int chid, coid, rcvid;
    char buf[1], detail[96];

    if (!test_chan_open(RTMUX_FLG_CHF_UNBLOCK, &chid, &coid)) {
        test_fail(name, "channel setup failed");
        return;
    }
    client.id = coid;
    if (pthread_create(&tc, NULL, client_main, &client) != 0) {
        test_fail(name, "client thread");
        return;
    }
    rcvid = msg_receive(chid, buf, sizeof buf, NULL);

    /* Let the client's REPLY timeout expire: on this channel the kernel then
     * queues an unblock notification and leaves the client blocked.
     */
    sleep_ms(10 * UNBLOCK_MS);
    if (rcvid == -1 || atomic_load(&client.done) ||
        inject_run("preempt-reply-unblock", pid, NULL) != 0) {
        test_fail(name, "receive or arm failed, or the client left early");
        return;
    }
    /* From a worker, so that a reply which never returns fails the case. */
    replier.id = rcvid;
    replier.status = REPLY_STATUS;
    if (pthread_create(&tr, NULL, replier_main, &replier) != 0 ||
        !wait_done(&replier)) {
        test_fail(name, "the reply did not return");
        return;
    }
    (void) pthread_join(tr, NULL);
    snprintf(detail, sizeof detail, "reply rc=%d errno=%d", replier.rc,
             replier.rc ? replier.err : 0);
    check(replier.rc == 0, "reply_unblock_restart reply completes", detail);
    check(inject_run("site-wait", "reply-unblock", NULL) == 0,
          "reply_unblock_restart restart taken",
          "the reply did not remove an unblock notification");
    if (!wait_done(&client)) {
        test_fail(name, "the client stayed reply-blocked");
        return;
    }
    (void) pthread_join(tc, NULL);
    snprintf(detail, sizeof detail, "send rc=%d errno=%d", client.rc,
             client.err);
    check(client.rc == REPLY_STATUS, name, detail);
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
}

static void *reader_main(void *raw)
{
    struct call *c = raw;
    char buf[1];

    if (c->pin_mask)
        (void) test_pin_mask(c->pin_mask);
    c->rc = msg_read(c->id, buf, sizeof buf, 0);
    c->err = errno;
    atomic_store(&c->done, 1);
    return NULL;
}

/* A wake that finds the client claimed is handed to the claimer, which
 * delivers it when it gives the claim up. The hand-over and the giving up
 * must exclude each other: here a MsgRead holds its claim while the client's
 * REPLY timeout fires, and is let go at the moment the timeout has seen the
 * claim and not yet left its wake behind. The client must time out.
 */
static void claim_release_keeps_wake(const char *pid)
{
    static const char name[] = "claim_release_keeps_wake";
    struct call client = {.timeout_ms = 300};
    struct call reader = {.pin_mask = 2}; /* off the hart the timeout is on */
    pthread_t tc, tr;
    int chid, coid, rcvid;
    char buf[1], rep = 0, detail[96];

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        test_skip(name, "needs two harts");
        return;
    }
    if (test_pin_mask(1) != 0 || !test_chan_open(0, &chid, &coid)) {
        test_fail(name, "pin or channel setup failed");
        return;
    }
    client.id = coid;
    if (inject_run("stall-claim-held", pid, "5000") != 0 ||
        inject_run("stall-force-claimed", pid, "50") != 0 ||
        pthread_create(&tc, NULL, client_main, &client) != 0) {
        test_fail(name, "arm or client thread failed");
        return;
    }
    rcvid = msg_receive(chid, buf, sizeof buf, NULL);
    reader.id = rcvid;
    if (rcvid == -1 || pthread_create(&tr, NULL, reader_main, &reader) != 0) {
        test_fail(name, "receive or reader thread failed");
        return;
    }
    (void) wait_done(&reader); /* held until the timeout's force_ready */
    check(inject_run("site-wait", "force-claimed", NULL) == 0,
          "claim_release_keeps_wake timeout met the claim",
          "the timeout did not find the client claimed");
    if (!wait_done(&client)) {
        /* The wake was lost; a reply still ends the exchange. */
        (void) msg_reply(rcvid, 0, &rep, 1);
        (void) wait_done(&client);
        test_fail(name, "the client's timeout was lost");
    } else {
        snprintf(detail, sizeof detail, "send rc=%d errno=%d", client.rc,
                 client.err);
        check(client.rc == -1 && client.err == ETIMEDOUT, name, detail);
    }
    (void) pthread_join(tr, NULL);
    (void) pthread_join(tc, NULL);
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
    (void) test_pin_mask(~0U);
}

struct pick {
    int chid, coid;
    atomic_int go, done, stop;
    int rc, err;
    struct _notification got;
};

/* Waits for a notification and nothing else, then stays out of the way. */
static void *pick_drain(void *raw)
{
    struct pick *p = raw;

    (void) test_pin_mask(1);
    p->rc = msg_receive_notification(p->chid, &p->got, sizeof p->got, NULL);
    p->err = errno;
    atomic_store(&p->done, 1);
    while (!atomic_load(&p->stop)) /* awake, as a thread taken twice is */
        sleep_ms(1);
    return NULL;
}

static void *pick_sender(void *raw)
{
    struct pick *p = raw;
    char req = 'm', rep = 0;

    (void) test_pin_mask(2); /* the stall keeps this hart */
    while (!atomic_load(&p->go))
        sleep_ms(1);
    p->rc = msg_send(p->coid, &req, 1, &rep, 1);
    p->err = errno;
    atomic_store(&p->done, 1);
    return NULL;
}

/* The only waiting receiver takes notifications only. A send sees it, may not
 * claim it, and is held right after it dropped the channel lock; a
 * notification then wakes the receiver. Let go, the send must queue for a
 * real receive and not deliver to the thread it saw.
 */
static void send_skips_woken_notification_receiver(const char *pid)
{
    static const char name[] = "send_skips_woken_notification_receiver";
    static struct pick drain, sender; /* a peer left behind still uses them */
    struct stall_watch watch = {.pid = -1};
    pthread_t td, ts;
    char tid[24], buf = 0, detail[128];
    int chid, coid, rcvid, entered;

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        test_skip(name, "needs two harts");
        return;
    }
    if (test_pin_mask(1) != 0 || !test_chan_open(0, &chid, &coid)) {
        test_fail(name, "pin or channel setup failed");
        return;
    }
    drain = (struct pick) {.chid = chid};
    sender = (struct pick) {.coid = coid};
    if (pthread_create(&td, NULL, pick_drain, &drain) != 0) {
        test_fail(name, "thread setup failed");
        goto out;
    }
    if (pthread_create(&ts, NULL, pick_sender, &sender) != 0) {
        test_fail(name, "thread setup failed");
        goto out_drain;
    }
    snprintf(tid, sizeof tid, "%u", (unsigned) ts);
    if (!test_thread_blocked(td, STATE_RECEIVE) ||
        inject_run("stall-send-pick", pid, tid) != 0 ||
        stall_watch_start(&watch, "send-pick") != 0) {
        test_fail(name, "the drain did not wait, or inject refused to arm");
        /* Neither peer is joined: the destroy ends the drain's wait and
         * fails the send.
         */
        atomic_store(&sender.go, 1);
        goto out_drain;
    }
    atomic_store(&sender.go, 1);
    entered = stall_watch_entered(&watch);
    check(entered, "send_skips_woken_notification_receiver window taken",
          "the send never reached the injected window");

    /* The send has seen the drain. Wake the drain behind its back. */
    (void) msg_send_notification(coid, 10, _NOTIFICATION_CODE_MINAVAIL, 7);
    check(test_wait_flag(&drain.done, 1, WAIT_MS) && drain.rc == 0 &&
              drain.got.value.sival_int == 7,
          "send_skips_woken_notification_receiver drain woken",
          "the notification did not reach the drain");
    stall_watch_finish(&watch);

    /* The message is for a receiver that takes messages, and waits for one. */
    test_receive_within(2000000000ULL);
    rcvid = msg_receive(chid, &buf, 1, NULL);
    snprintf(detail, sizeof detail, "receive=%d byte=%d, drain value=%d", rcvid,
             buf, drain.got.value.sival_int);
    check(rcvid > 0 && buf == 'm' && drain.got.value.sival_int == 7, name,
          detail);
    if (rcvid > 0)
        (void) msg_reply(rcvid, 0, &buf, 1);
    if (test_wait_flag(&sender.done, 1, WAIT_MS)) {
        snprintf(detail, sizeof detail, "send rc=%d errno=%d", sender.rc,
                 sender.rc == -1 ? sender.err : 0);
        check(sender.rc == 0, "send_skips_woken_notification_receiver replied",
              detail);
        (void) pthread_join(ts, NULL);
    } else {
        test_fail(name, "the send did not return");
    }
    if (atomic_load(&drain.done)) {
        atomic_store(&drain.stop, 1);
        (void) pthread_join(td, NULL);
    }
out_drain:
    atomic_store(&drain.stop, 1);
out:
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
    (void) test_pin_mask(~0U);
}

struct two_sends {
    int coid;
    atomic_int stage; /* 0, 2: send now; 1, 3: that send returned */
    int rc[2], err[2];
};

static void *two_sends_main(void *raw)
{
    struct two_sends *t = raw;

    for (int i = 0; i < 2; i++) {
        uint64_t ns = UNBLOCK_MS * 1000000ULL;
        char req = (char) i, rep[REPLY_STATUS];

        (void) test_wait_flag(&t->stage, 2 * i, WAIT_MS);
        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_REPLY, NULL, &ns,
                               NULL);
        t->rc[i] = msg_send(t->coid, &req, 1, rep, sizeof rep);
        t->err[i] = errno;
        atomic_store(&t->stage, 2 * i + 1);
    }
    return NULL;
}

/* Receive on chid for ms milliseconds at most: the rcvid, 0 for a
 * notification (in *n), or -1.
 */
static int receive_ms(int chid, unsigned ms, struct _notification *n)
{
    test_receive_within(ms * 1000000ULL);
    return msg_receive(chid, n, sizeof *n, NULL);
}

/* A REPLY timeout that found its client unclaimed is held until a reply
 * claims the client; the reply, past its look for a queued unblock
 * notification, is held until the timeout has marked one queued (or 3 s).
 * Whatever the timeout does in that window must not outlive the exchange: the
 * client's next request must still get its unblock notification when its own
 * timeout fires.
 */
static void reply_unblock_race(const char *pid)
{
    static const char name[] = "reply_unblock_race";
    static struct two_sends client; /* a client left behind still uses it */
    struct _notification n;
    pthread_t tc;
    struct timespec t0, t1;
    int chid, coid, rcvid, stale = 0, unblocked = 0;
    long reply_ms;
    char rep = 0, detail[160];

    if (sysconf(_SC_NPROCESSORS_ONLN) < 3) {
        test_skip(name, "needs three harts");
        return;
    }
    /* The client, and so the hart its timeout fires on, is hart 1; the
     * replier is hart 2.
     */
    if (test_pin_mask(2) != 0 ||
        !test_chan_open(RTMUX_FLG_CHF_UNBLOCK, &chid, &coid)) {
        test_fail(name, "pin or channel setup failed");
        return;
    }
    client = (struct two_sends) {.coid = coid};
    if (inject_run("stall-unblock-race", pid, HOLD_MS) != 0 ||
        pthread_create(&tc, NULL, two_sends_main, &client) != 0) {
        test_fail(name, "arm or client thread failed");
        goto out;
    }
    (void) test_pin_mask(4);
    rcvid = receive_ms(chid, WAIT_MS, &n);
    if (rcvid <= 0) {
        test_fail(name, "no first request");
        goto out;
    }
    /* Reply once the timeout is held in its window: SPIN_MS after the request,
     * far past its 20 ms deadline and well inside the hold. A spawn of
     * /bin/inject to ask first would take longer than the hold under TCG, and
     * the replier spins instead of sleeping because an idle hart may run the
     * timer expiry, and the hold would then keep it off its hart.
     */
    (void) clock_gettime(CLOCK_MONOTONIC, &t0);
    do
        (void) clock_gettime(CLOCK_MONOTONIC, &t1);
    while ((t1.tv_sec - t0.tv_sec) * 1000 +
               (t1.tv_nsec - t0.tv_nsec) / 1000000 <
           SPIN_MS);
    (void) clock_gettime(CLOCK_MONOTONIC, &t0);
    (void) msg_reply(rcvid, REPLY_STATUS, &rep, 1);
    (void) clock_gettime(CLOCK_MONOTONIC, &t1);
    reply_ms = (t1.tv_sec - t0.tv_sec) * 1000 +
               (t1.tv_nsec - t0.tv_nsec) / 1000000;
    check(inject_run("site-wait", "unblock-force", NULL) == 0,
          "reply_unblock_race timeout held in the window",
          "the client's timeout never reached the injected window");
    (void) test_wait_flag(&client.stage, 1, WAIT_MS);
    snprintf(detail, sizeof detail, "first send rc=%d errno=%d", client.rc[0],
             client.rc[0] == -1 ? client.err[0] : 0);
    check(client.rc[0] == REPLY_STATUS, "reply_unblock_race reply delivered",
          detail);
    check(inject_run("site-wait", "unblock-reply", NULL) == 0,
          "reply_unblock_race reply held in the window",
          "the reply never reached the injected window");

    /* A notification for the answered request is harmless; count it. */
    while (receive_ms(chid, 50, &n) == 0)
        if (n.code == _NOTIFICATION_CODE_UNBLOCK)
            stale++;

    atomic_store(&client.stage, 2);
    /* A stale notification that came after the drain above is not the
     * second request; count it and keep receiving.
     */
    while ((rcvid = receive_ms(chid, WAIT_MS, &n)) == 0)
        if (n.code == _NOTIFICATION_CODE_UNBLOCK)
            stale++;
    if (rcvid < 0) {
        test_fail(name, "no second request");
        goto out;
    }
    while (!unblocked && receive_ms(chid, 10 * UNBLOCK_MS + 500, &n) == 0)
        unblocked = n.code == _NOTIFICATION_CODE_UNBLOCK &&
                    n.value.sival_int == rcvid;
    (void) msg_error(rcvid, EINTR);
    (void) test_wait_flag(&client.stage, 3, WAIT_MS);
    snprintf(detail, sizeof detail,
             "second request unblocked=%d, stale notifications=%d, second "
             "send rc=%d errno=%d, first reply took %ld ms",
             unblocked, stale, client.rc[1], client.err[1], reply_ms);
    printf("reply_unblock_race: %s\n", detail);
    check(unblocked && client.rc[1] == -1 && client.err[1] == EINTR, name,
          detail);
    if (atomic_load(&client.stage) == 3)
        (void) pthread_join(tc, NULL);
out:
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
    (void) test_pin_mask(~0U);
}

int test_reply_claim(void)
{
    char pid[16];

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    reply_unblock_restart(pid);
    reply_detached_connection(pid);
    claim_release_keeps_wake(pid);
    send_skips_woken_notification_receiver(pid);
    reply_unblock_race(pid);
    return 0;
}
