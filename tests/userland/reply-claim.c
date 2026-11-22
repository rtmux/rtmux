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
 *
 * Both windows are a few instructions wide, so the cases hold them open with
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

#include "test.h"

#define WAIT_MS 5000    /* the longest any wait in a case may take */
#define STALL_MS "3000" /* the stall's own limit, if no destroy ends it */
#define UNBLOCK_MS 20
#define REPLY_STATUS 77

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
    for (int i = 0; i < WAIT_MS && !atomic_load(&c->done); i++)
        sleep_ms(1);
    return atomic_load(&c->done) != 0;
}

static bool chan_open(unsigned flags, int *chid, int *coid)
{
    *coid = -1;
    *chid = channel_create(flags);
    if (*chid != -1)
        *coid = connect_attach(0, getpid(), *chid, RTMUX_SIDE_CHANNEL, 0);
    return *coid != -1;
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
    if (test_pin_mask(1) != 0 || !chan_open(0, &chid, &coid)) {
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

    if (!chan_open(RTMUX_FLG_CHF_UNBLOCK, &chid, &coid)) {
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
    if (test_pin_mask(1) != 0 || !chan_open(0, &chid, &coid)) {
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

int test_reply_claim(void)
{
    char pid[16];

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    reply_unblock_restart(pid);
    reply_detached_connection(pid);
    claim_release_keeps_wake(pid);
    return 0;
}
