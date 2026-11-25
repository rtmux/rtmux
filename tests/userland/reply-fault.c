/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * reply-fault — a reply whose source the kernel cannot read.
 *
 * The server's reply names memory that is not there: the buffer, or the iov
 * array itself. The reply fails with EFAULT, and so does the client's send:
 * the kernel fails the client itself, since nothing else would ever answer
 * it. The connection is as good as before for the next exchange.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/rtmux-core.h>

#include "test.h"

#define RF_WAIT_MS 5000

struct rf_client {
    int coid;
    atomic_int sent;
    int rc, err;
    char answer;
};

static void *rf_client_main(void *raw)
{
    struct rf_client *c = raw;
    char req = 'q', rep[64] = {0};

    c->rc = msg_send(c->coid, &req, 1, rep, sizeof rep);
    c->err = errno;
    c->answer = rep[0];
    atomic_store(&c->sent, 1);
    return NULL;
}

/* One exchange. kind 0: the reply's buffer is unmapped; 1: its iov array is;
 * 2: a good reply. Returns false when the next exchange must not run: a
 * client left blocked would hand it a stale request and write over the state
 * it shares.
 */
static bool rf_exchange(int chid, int coid, int kind, void *bad)
{
    static const char *const names[] = {"reply_fault unmapped buffer",
                                        "reply_fault unmapped iov array",
                                        "reply_fault good reply afterwards"};
    static struct rf_client client; /* a client left behind still uses it */
    pthread_t tc;
    char req = 0, good = 'g', detail[128];
    int rcvid, rc, err;

    client = (struct rf_client) {.coid = coid};
    if (pthread_create(&tc, NULL, rf_client_main, &client) != 0) {
        test_fail(names[kind], "client thread");
        return false;
    }
    test_receive_within(RF_WAIT_MS * 1000000ULL);
    rcvid = msg_receive(chid, &req, 1, NULL);
    if (rcvid == -1) {
        test_fail(names[kind], "the request was not received");
        return false; /* the client is ended by the channel's destroy */
    }
    errno = 0;
    if (kind == 0)
        rc = msg_reply(rcvid, 0, bad, 64);
    else if (kind == 1)
        rc = msg_replyv(rcvid, 0, (const iov_t *) bad, 2);
    else
        rc = msg_reply(rcvid, 0, &good, 1);
    err = errno;
    if (!test_wait_flag(&client.sent, 1, RF_WAIT_MS)) {
        /* Left in REPLY by a reply that failed: end the exchange so that the
         * thread can be joined, and say so.
         */
        (void) msg_error(rcvid, ECANCELED);
        test_fail(names[kind],
                  "the client was left blocked by the failed reply");
        if (!test_wait_flag(&client.sent, 1, RF_WAIT_MS))
            return false;
    } else {
        snprintf(detail, sizeof detail,
                 "reply rc=%d errno=%d, send rc=%d errno=%d answer=%d", rc,
                 rc == -1 ? err : 0, client.rc,
                 client.rc == -1 ? client.err : 0, client.answer);
        check(kind == 2 ? rc == 0 && client.rc == 0 && client.answer == 'g'
                        : rc == -1 && err == EFAULT && client.rc == -1 &&
                              client.err == EFAULT,
              names[kind], detail);
    }
    (void) pthread_join(tc, NULL);
    return true;
}

void test_reply_fault(void)
{
    void *bad = test_unmapped_user_addr();
    int chid, coid;

    if (!bad) {
        test_skip("reply_fault", "no unmapped address to reply from");
        return;
    }
    if (!test_chan_open(0, &chid, &coid)) {
        test_fail("reply_fault", "channel setup failed");
        return;
    }
    for (int kind = 0; kind < 3; kind++)
        if (!rf_exchange(chid, coid, kind, bad))
            break;
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
}
