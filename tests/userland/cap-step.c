/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * cap-step — a long message with a native capability header.
 *
 * Such a message is copied in steps like any other, and its header is dealt
 * with afterwards, from a snapshot the kernel takes of the sender's copy and
 * then writes over the head of what the receiver was given.
 *
 * - cap_long_stepped: a 256 KiB message that names an endpoint capability
 *   arrives whole, with the capability installed (the id rewritten), after
 *   more than one kernel entry.
 * - cap_header_snapshot: the sender's header is rewritten by a sibling after
 *   the bytes were copied and before the kernel looked at it. The receiver
 *   must see the header the kernel examined, not the one that was copied.
 * - cap_header_sparse: a message whose head lies behind a long run of empty
 *   iov entries is refused, since the kernel will not walk them to look.
 *
 * The first two need the debug fault-injection call (/bin/inject,
 * CONFIG_DEBUG_INJECT kernels only).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>

#include "../../kernel/include/private/inject.h"
#include "test.h"

#define CS_BYTES (256 * 1024)
#define CS_WAIT_MS 10000
#define CS_SPARSE 100 /* empty entries ahead of the data: over the limit */
#define CS_BOGUS_CAP 0x7ead00000badULL

static unsigned char cs_out[CS_BYTES], cs_in[CS_BYTES];

struct cs_send {
    int coid;
    unsigned pin_mask;
    iov_t *iov; /* NULL: the flat buffer cs_out, len bytes of it */
    int parts;
    size_t len;
    bool gated; /* wait for go before sending */
    atomic_int started, go, done;
    int rc, err;
};

struct cs_recv {
    int chid;
    atomic_int done;
    int rcvid;
};

static void *cs_receiver(void *raw)
{
    struct cs_recv *r = raw;

    r->rcvid = msg_receive(r->chid, cs_in, CS_BYTES, NULL);
    atomic_store(&r->done, 1);
    return NULL;
}

static void *cs_sender(void *raw)
{
    struct cs_send *s = raw;
    char rep;

    if (s->pin_mask)
        (void) test_pin_mask(s->pin_mask);
    atomic_store(&s->started, 1);
    while (s->gated && !atomic_load(&s->go))
        sleep_ms(1);
    s->rc = s->iov ? msg_sendv(s->coid, s->iov, s->parts, NULL, 0)
                   : msg_send(s->coid, cs_out, s->len, &rep, 1);
    s->err = errno;
    atomic_store(&s->done, 1);
    return NULL;
}

static bool cs_wait(atomic_int *flag)
{
    for (int i = 0; i < CS_WAIT_MS && !atomic_load(flag); i++)
        sleep_ms(1);
    return atomic_load(flag) != 0;
}

static void cs_fill(uint64_t cap)
{
    struct _io_native_hdr hdr;

    for (int i = 0; i < CS_BYTES; i++)
        cs_out[i] = (unsigned char) (i * 11 + 7);
    memset(&hdr, 0, sizeof hdr);
    hdr.msg.type = _IO_MSG;
    hdr.msg.protocol = _IOMSG_RTMUX_NATIVE;
    hdr.version = _IO_NATIVE_VERSION;
    hdr.nxfers = 1;
    hdr.xfers[0].id = cap;
    hdr.xfers[0].rights = RTMUX_CAP_ENDPOINT_SEND;
    memcpy(cs_out, &hdr, sizeof hdr);
}

static int cs_stat(unsigned counter)
{
    char arg[8];
    int n;

    snprintf(arg, sizeof arg, "%u", counter);
    n = inject_run("xfer-stats", arg, NULL);
    return n == 255 ? -1 : n;
}

/* The receiver waits first, so the sender's kernel call does the copy. */
static void cap_long_stepped(int chid, int coid, uint64_t cap, const char *pid)
{
    static const char name[] = "cap_long_stepped";
    struct cs_send snd = {.coid = coid, .len = CS_BYTES};
    struct _io_native_hdr *got = (void *) cs_in;
    pthread_t ts;
    char detail[128];
    int rcvid, yields;

    cs_fill(cap);
    memset(cs_in, 0, sizeof cs_in);
    if (inject_run("xfer-trace", pid, NULL) != 0 ||
        pthread_create(&ts, NULL, cs_sender, &snd) != 0) {
        test_fail(name, "arm or sender thread failed");
        return;
    }
    rcvid = msg_receive(chid, cs_in, CS_BYTES, NULL);
    yields = cs_stat(INJECT_XFER_YIELDS);
    snprintf(detail, sizeof detail, "rcvid=%d yields=%d nxfers=%u id=%#llx",
             rcvid, yields, got->nxfers, (unsigned long long) got->xfers[0].id);
    check(rcvid != -1 && yields >= 1, "cap_long_stepped copied in steps",
          detail);
    check(rcvid != -1 && got->nxfers == 1 && got->xfers[0].id != 0 &&
              got->xfers[0].id != cap &&
              memcmp(cs_in + sizeof *got, cs_out + sizeof *got,
                     CS_BYTES - sizeof *got) == 0,
          name, detail);
    if (rcvid != -1)
        (void) msg_reply(rcvid, 0, NULL, 0);
    (void) cs_wait(&snd.done);
    (void) pthread_join(ts, NULL);
}

static void cap_header_snapshot(int chid, int coid, const char *pid)
{
    static const char name[] = "cap_header_snapshot";
    struct cs_send snd = {
        .coid = coid, .pin_mask = 2, .len = 8192, .gated = true};
    struct cs_recv rcv = {.chid = chid, .rcvid = -1};
    struct _io_native_hdr *sent = (void *) cs_out, *got = (void *) cs_in;
    struct stall_watch watch = {.pid = -1};
    pthread_t ts, tr;
    char tid[24], detail[128];
    bool entered;

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        test_skip(name, "needs two harts");
        return;
    }

    /* The header that is copied names a capability the sender does not
     * hold. Nothing may hand that id to the receiver as an installed one.
     */
    cs_fill(CS_BOGUS_CAP);
    memset(cs_in, 0, sizeof cs_in);
    if (test_pin_mask(1) != 0 ||
        pthread_create(&tr, NULL, cs_receiver, &rcv) != 0 ||
        pthread_create(&ts, NULL, cs_sender, &snd) != 0 ||
        !cs_wait(&snd.started)) {
        test_fail(name, "pin or thread setup failed");
        return;
    }
    sleep_ms(50); /* the receiver is waiting: the send does the copy */
    snprintf(tid, sizeof tid, "%u", (unsigned) ts);
    if (inject_run("stall-cap-peek", pid, tid) != 0 ||
        stall_watch_start(&watch, "cap-peek") != 0) {
        test_fail(name, "inject refused to arm");
        return;
    }
    atomic_store(&snd.go, 1);

    /* The bytes are in the receiver's buffer and the kernel has not looked
     * at the header yet: take the transfer out of the sender's copy.
     */
    entered = stall_watch_entered(&watch);
    sent->nxfers = 0;
    stall_watch_finish(&watch);
    check(entered, "cap_header_snapshot window taken",
          "the send never reached the injected window");
    if (!cs_wait(&rcv.done)) {
        test_fail(name, "the receive did not return");
        return;
    }
    snprintf(detail, sizeof detail, "rcvid=%d nxfers=%u id=%#llx", rcv.rcvid,
             got->nxfers, (unsigned long long) got->xfers[0].id);
    check(rcv.rcvid != -1 && got->nxfers == 0, name, detail);
    if (rcv.rcvid != -1)
        (void) msg_reply(rcv.rcvid, 0, NULL, 0);
    (void) cs_wait(&snd.done);
    (void) pthread_join(ts, NULL);
    (void) pthread_join(tr, NULL);
    (void) test_pin_mask(~0U);
}

/* A head behind CS_SPARSE empty entries: refused, and the receiver left
 * waiting. Behind a few, delivered.
 */
static void cap_header_sparse(int chid, int coid)
{
    static const char name[] = "cap_header_sparse";
    static iov_t iov[CS_SPARSE + 1];
    struct cs_send snd = {.coid = coid, .iov = iov};
    pthread_t ts;
    char detail[96];
    int rcvid;

    for (int i = 0; i < CS_BYTES; i++)
        cs_out[i] = (unsigned char) (i * 3 + 1);
    for (int lead = CS_SPARSE; lead >= 8; lead -= CS_SPARSE - 8) {
        for (int i = 0; i < lead; i++)
            SETIOV(&iov[i], cs_out, 0);
        SETIOV(&iov[lead], cs_out, 4096);
        snd.parts = lead + 1;
        atomic_store(&snd.done, 0);
        memset(cs_in, 0, 4096);
        if (pthread_create(&ts, NULL, cs_sender, &snd) != 0) {
            test_fail(name, "sender thread");
            return;
        }
        if (lead == CS_SPARSE) {
            /* With no receiver waiting the send would queue: give it one,
             * with a timeout, and expect the send to fail instead.
             */
            uint64_t ns = 500000000ULL;

            (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL,
                                   &ns, NULL);
            rcvid = msg_receive(chid, cs_in, 4096, NULL);
            (void) cs_wait(&snd.done);
            snprintf(detail, sizeof detail, "receive=%d send rc=%d errno=%d",
                     rcvid, snd.rc, snd.err);
            check(rcvid == -1 && snd.rc == -1 && snd.err == EINVAL,
                  "cap_header_sparse refused", detail);
            if (rcvid != -1)
                (void) msg_reply(rcvid, 0, NULL, 0);
        } else {
            rcvid = msg_receive(chid, cs_in, 4096, NULL);
            snprintf(detail, sizeof detail, "receive=%d", rcvid);
            check(rcvid != -1 && memcmp(cs_in, cs_out, 4096) == 0, name,
                  detail);
            if (rcvid != -1)
                (void) msg_reply(rcvid, 0, NULL, 0);
            (void) cs_wait(&snd.done);
        }
        (void) pthread_join(ts, NULL);
    }
}

int test_cap_step(void)
{
    struct _server_info info;
    char pid[16];
    int chid, coid;
    bool inject = inject_run("alloc-status", NULL, NULL) == 0;

    chid = channel_create(0);
    coid = chid == -1
               ? -1
               : connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    memset(&info, 0, sizeof info);
    if (coid == -1 || connect_server_info(0, (uint64_t) coid, &info) != coid ||
        info.endpoint_cap == 0) {
        test_fail("cap_step setup", "channel, connection or endpoint cap");
        return 0;
    }
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    cap_header_sparse(chid, coid);
    if (inject) {
        cap_long_stepped(chid, coid, info.endpoint_cap, pid);
        cap_header_snapshot(chid, coid, pid);
    } else {
        test_skip("cap_long_stepped", "fault injection unavailable");
        test_skip("cap_header_snapshot", "fault injection unavailable");
    }
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
    return 0;
}
