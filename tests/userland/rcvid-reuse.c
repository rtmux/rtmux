/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * rcvid-reuse — a server acting late on a receive id.
 *
 * A receive id is a thread number and a connection slot, so the next request
 * of the same client thread on the same connection has the same one. A server
 * thread that was given the id for one request and acts on it after that
 * request has ended must be refused, and must leave the request that is
 * blocked under the id now alone.
 *
 * - rcvid_stale_unblock: a server thread takes the unblock notification of a
 *   request and sits on it. The request is answered and the client sends the
 *   next one. The thread's msg_info and msg_error on the id then fail with
 *   ESRCH; they used to reach the new request and fail it with EINTR.
 * - rcvid_stale_reply: a server thread receives a request that then times
 *   out, and replies after the client's next request is blocked under the
 *   same id. The reply fails with ESRCH; it used to answer the new request.
 * - rcvid_handed_on: a server thread receives a request that another thread
 *   answers, and is then handed the client's next request, which it did not
 *   receive. Its reply must go through: a thread whose request was answered
 *   elsewhere is not held to it.
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

#define RR_WAIT_MS 5000
#define RR_TIMEOUT_MS 100

enum rr_mode { RR_UNBLOCK, RR_TIMEOUT, RR_HANDED_ON, RR_MODES };

struct rr_client {
    int coid;
    bool timeout;        /* the first send has a REPLY timeout */
    atomic_int go, sent; /* go: requests it may send; sent: sends returned */
    int rc[2], err[2];
    char answer[2]; /* the byte each send was answered with */
};

/* Two requests on one connection from one thread: the first with a REPLY
 * timeout, the second, when it is told to, without.
 */
static void *rr_client_main(void *raw)
{
    struct rr_client *c = raw;
    char req = 'q';

    for (int i = 0; i < 2; i++) {
        while (atomic_load(&c->go) <= i)
            sleep_ms(1);
        if (i == 0 && c->timeout) {
            uint64_t ns = RR_TIMEOUT_MS * 1000000ULL;

            (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_REPLY, NULL,
                                   &ns, NULL);
        }
        c->rc[i] = msg_send(c->coid, &req, 1, &c->answer[i], 1);
        c->err[i] = errno;
        atomic_fetch_add(&c->sent, 1);
    }
    return NULL;
}

struct rr_server {
    int chid;
    bool notification; /* take a notification, not a message */
    atomic_int taken, act, acted;
    int rcvid;
    int info_rc, info_err; /* msg_info on the id, late */
    int end_rc, end_err;   /* msg_error (notification) or msg_reply, late */
};

/* The server thread that is given the receive id and acts on it late: the
 * kernel holds a thread to what that thread itself received.
 */
static void *rr_server_main(void *raw)
{
    struct rr_server *s = raw;
    struct _notification n;
    struct _msg_info info;
    char req, rep = 'L'; /* the late answer */
    uint64_t ns = RR_WAIT_MS * 1000000ULL;

    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &ns,
                           NULL);
    if (s->notification) {
        s->rcvid = msg_receive_notification(s->chid, &n, sizeof n, NULL) == 0 &&
                           n.code == _NOTIFICATION_CODE_UNBLOCK
                       ? n.value.sival_int
                       : -1;
    } else {
        s->rcvid = msg_receive(s->chid, &req, 1, NULL);
    }
    atomic_store(&s->taken, 1);
    while (!atomic_load(&s->act))
        sleep_ms(1);
    if (s->rcvid != -1) {
        s->info_rc = msg_info(s->rcvid, &info);
        s->info_err = errno;
        s->end_rc = s->notification ? msg_error(s->rcvid, EINTR)
                                    : msg_reply(s->rcvid, 0, &rep, 1);
        s->end_err = errno;
    }
    atomic_store(&s->acted, 1);
    return NULL;
}

static bool rr_wait(atomic_int *flag, int want)
{
    for (int i = 0; i < RR_WAIT_MS && atomic_load(flag) < want; i++)
        sleep_ms(1);
    return atomic_load(flag) >= want;
}

static int rr_receive(int chid)
{
    char req;
    uint64_t ns = RR_WAIT_MS * 1000000ULL;

    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &ns,
                           NULL);
    return msg_receive(chid, &req, 1, NULL);
}

static bool rr_open(unsigned flags, int *chid, int *coid)
{
    *coid = -1;
    *chid = channel_create(flags);
    if (*chid != -1)
        *coid = connect_attach(0, getpid(), *chid, RTMUX_SIDE_CHANNEL, 0);
    return *coid != -1;
}

/* One case. The late thread is given the id of the client's first request:
 * by its unblock notification (RR_UNBLOCK, this thread having received the
 * request itself), or by receiving the request. The first request then ends
 * behind its back, timed out (RR_TIMEOUT) or answered by this thread, the
 * client's second request blocks under the same id, and the late thread acts
 * on the id.
 */
static void rr_case(enum rr_mode mode)
{
    static const char *const names[RR_MODES] = {
        "rcvid_stale_unblock", "rcvid_stale_reply", "rcvid_handed_on"};
    static const char *const reused[RR_MODES] = {
        "rcvid_stale_unblock id reused", "rcvid_stale_reply id reused",
        "rcvid_handed_on id reused"};
    static const char *const verdict[RR_MODES] = {"rcvid_stale_unblock refused",
                                                  "rcvid_stale_reply refused",
                                                  "rcvid_handed_on allowed"};
    static const char *const outcome[RR_MODES] = {
        "rcvid_stale_unblock new request untouched",
        "rcvid_stale_reply new request untouched",
        "rcvid_handed_on answered by the late thread"};
    /* One set for each case: a thread a failed case leaves behind keeps
     * using its own.
     */
    static struct rr_client clients[RR_MODES];
    static struct rr_server lates[RR_MODES];
    struct rr_client *client = &clients[mode];
    struct rr_server *late = &lates[mode];
    const char *name = names[mode];
    pthread_t tc, ts;
    char one = '1', two = '2', detail[160];
    int chid, coid, first = -1, second = -1;
    bool refused, started_late = false;

    if (!rr_open(mode == RR_UNBLOCK ? RTMUX_FLG_CHF_UNBLOCK : 0, &chid,
                 &coid)) {
        test_fail(name, "channel setup failed");
        return;
    }
    *client =
        (struct rr_client) {.coid = coid, .timeout = mode != RR_HANDED_ON};
    *late =
        (struct rr_server) {.chid = chid, .notification = mode == RR_UNBLOCK};
    if (pthread_create(&tc, NULL, rr_client_main, client) != 0) {
        test_fail(name, "client thread");
        goto out;
    }
    if (mode == RR_UNBLOCK) {
        /* This thread has the request; the client's timeout then asks for
         * an unblock, which the late thread takes.
         */
        atomic_store(&client->go, 1);
        first = rr_receive(chid);
        if (first == -1) {
            test_fail(name, "first receive");
            goto out;
        }
    }
    if (pthread_create(&ts, NULL, rr_server_main, late) != 0) {
        test_fail(name, "server thread");
        goto out;
    }
    started_late = true;
    if (mode != RR_UNBLOCK) {
        /* The late thread is waiting, so it is the one that gets the id. */
        if (!test_thread_blocked(ts, STATE_RECEIVE))
            test_fail(name, "the late server did not wait");
        atomic_store(&client->go, 1);
    }
    if (!rr_wait(&late->taken, 1) || late->rcvid == -1 ||
        (mode == RR_UNBLOCK && late->rcvid != first)) {
        snprintf(detail, sizeof detail, "request=%d, the late server got %d",
                 first, late->rcvid);
        test_fail(name, detail);
        goto out;
    }
    first = late->rcvid;

    /* The first request ends behind the late thread's back: answered here,
     * or timed out. The client's next one blocks under the same id.
     */
    if (mode != RR_TIMEOUT)
        (void) msg_reply(first, 0, &one, 1);
    if (!rr_wait(&client->sent, 1)) {
        test_fail(name, "the first send did not return");
        goto out;
    }
    atomic_store(&client->go, 2);
    second = rr_receive(chid);
    snprintf(detail, sizeof detail,
             "first send rc=%d errno=%d answer=%d, ids %d then %d",
             client->rc[0], client->rc[0] == -1 ? client->err[0] : 0,
             client->answer[0], first, second);
    check(second == first &&
              (mode == RR_TIMEOUT
                   ? client->rc[0] == -1 && client->err[0] == ETIMEDOUT
                   : client->rc[0] == 0 && client->answer[0] == '1'),
          reused[mode], detail);

    /* Now the late thread acts on what it was given. */
    atomic_store(&late->act, 1);
    if (!rr_wait(&late->acted, 1)) {
        test_fail(name, "the late server did not return");
        goto out;
    }
    (void) pthread_join(ts, NULL);
    started_late = false;
    snprintf(detail, sizeof detail,
             "msg_info rc=%d errno=%d, %s rc=%d errno=%d", late->info_rc,
             late->info_rc == -1 ? late->info_err : 0,
             mode == RR_UNBLOCK ? "msg_error" : "msg_reply", late->end_rc,
             late->end_rc == -1 ? late->end_err : 0);
    refused = late->info_rc == -1 && late->info_err == ESRCH &&
              late->end_rc == -1 && late->end_err == ESRCH;
    check(mode == RR_HANDED_ON ? late->info_rc == 0 && late->end_rc == 0
                               : refused,
          verdict[mode], detail);

    /* The second request is answered by the thread that received it, unless
     * it was handed to the late thread.
     */
    if (second != -1 && mode != RR_HANDED_ON)
        (void) msg_reply(second, 0, &two, 1);
    if (rr_wait(&client->sent, 2)) {
        snprintf(detail, sizeof detail, "second send rc=%d errno=%d answer=%d",
                 client->rc[1], client->rc[1] == -1 ? client->err[1] : 0,
                 client->answer[1]);
        check(client->rc[1] == 0 &&
                  client->answer[1] == (mode == RR_HANDED_ON ? 'L' : '2'),
              outcome[mode], detail);
        (void) pthread_join(tc, NULL);
    } else {
        test_fail(name, "the second send did not return");
    }
out:
    /* A peer that is still blocked is ended by the destroy, and one that
     * waits for its cue gets it; neither is joined.
     */
    if (started_late)
        atomic_store(&late->act, 1);
    atomic_store(&client->go, 2);
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
}

int test_rcvid_reuse(void)
{
    for (int mode = 0; mode < RR_MODES; mode++)
        rr_case((enum rr_mode) mode);
    return 0;
}
