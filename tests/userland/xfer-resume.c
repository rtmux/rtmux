/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * xfer-resume — a stepped message copy resumes where it stopped.
 *
 * A receive of a queued long message copies it a step per kernel entry and
 * then, once, looks at its head for a native capability header through a
 * transfer of its own, which borrows the restart links and the byte count of
 * the copy and puts them back. The copy must not begin again from byte 0.
 *
 * The case counts what the copies of this process do with the debug
 * fault-injection call (/bin/inject xfer-trace, CONFIG_DEBUG_INJECT kernels
 * only): the copy must be stepped, start once, and have its head looked at.
 * The number of steps is printed for the record, for the receive and for a
 * reply of the same size.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>

#include "../../kernel/include/private/inject.h"
#include "test.h"

/* Large enough that the copy outlasts one step's time budget (XFER_BUDGET_US)
 * on any host: 256 KiB could go in one step on a fast one, and then nothing
 * was stepped to check. Mapped when the case runs, not in the binary's bss,
 * which every spawn of it would zero. */
#define XR_BYTES (4 * 1024 * 1024)
#define XR_WAIT_MS 10000

/* One counter of INJECT_OP_XFER_STATS; -1 if it could not be read. */
static int xr_stat(unsigned counter)
{
    char arg[8];
    int n;

    snprintf(arg, sizeof arg, "%u", counter);
    n = inject_run("xfer-stats", arg, NULL);
    return n == 255 ? -1 : n;
}

static unsigned char *xr_out, *xr_in, *xr_back;

struct xr_sender {
    int coid;
    atomic_int done;
    int rc;
};

static void *xr_sender_main(void *raw)
{
    struct xr_sender *s = raw;

    s->rc = msg_send(s->coid, xr_out, XR_BYTES, xr_back, XR_BYTES);
    atomic_store(&s->done, 1);
    return NULL;
}

static long long xr_now_us(void)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

int test_xfer_resume(void)
{
    static const char name[] = "xfer_resume";
    struct xr_sender snd = {0};
    pthread_t ts;
    char pid[16], detail[128];
    int chid, rcvid, starts, yields, peeks, budgets;
    long long t0, us;

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    xr_out = mmap(NULL, 3 * (size_t) XR_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (xr_out == MAP_FAILED) {
        test_fail(name, "could not map the buffers");
        return 0;
    }
    xr_in = xr_out + XR_BYTES;
    xr_back = xr_in + XR_BYTES;
    snprintf(pid, sizeof pid, "%d", (int) getpid());
    for (int i = 0; i < XR_BYTES; i++)
        xr_out[i] = (unsigned char) (i * 13 + 5);
    chid = channel_create(0);
    snd.coid = chid == -1
                   ? -1
                   : connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (snd.coid == -1 ||
        pthread_create(&ts, NULL, xr_sender_main, &snd) != 0) {
        test_fail(name, "channel or sender setup failed");
        return 0;
    }

    /* The sender queues first, so the receive below does the copy, from the
     * sender's address space, which is the path that peeks through xfermsg.
     */
    sleep_ms(100);
    if (inject_run("xfer-trace", pid, NULL) != 0) {
        test_fail(name, "inject refused to arm");
        return 0;
    }
    t0 = xr_now_us();
    rcvid = msg_receive(chid, xr_in, XR_BYTES, NULL);
    us = xr_now_us() - t0;
    starts = xr_stat(INJECT_XFER_STARTS);
    yields = xr_stat(INJECT_XFER_YIELDS);
    peeks = xr_stat(INJECT_XFER_PEEKS);
    budgets = xr_stat(INJECT_XFER_BUDGETS);
    snprintf(detail, sizeof detail,
             "rcvid=%d starts=%d yields=%d by-budget=%d peeks=%d", rcvid,
             starts, yields, budgets, peeks);
    printf("xfer-resume: receive bytes=%d us=%lld %s\n", XR_BYTES, us, detail);
    check(rcvid != -1 && memcmp(xr_in, xr_out, XR_BYTES) == 0,
          "xfer_resume bytes arrive", detail);
    check(yields >= 1 && peeks >= 1, "xfer_resume copy was stepped and peeked",
          detail); /* peeked: once, after the last step */
    check(starts == 1, name, detail);

    /* A reply of the same size, for the number of steps it takes. */
    if (rcvid == -1 || inject_run("xfer-trace", pid, NULL) != 0) {
        test_fail("xfer_resume reply", "no client, or inject refused to arm");
        return 0;
    }
    t0 = xr_now_us();
    if (msg_reply(rcvid, 0, xr_in, XR_BYTES) == -1) {
        test_fail("xfer_resume reply", "reply failed");
        return 0;
    }
    us = xr_now_us() - t0;
    yields = xr_stat(INJECT_XFER_YIELDS);
    budgets = xr_stat(INJECT_XFER_BUDGETS);
    for (int i = 0; i < XR_WAIT_MS && !atomic_load(&snd.done); i++)
        sleep_ms(1);
    if (!atomic_load(&snd.done)) {
        test_fail("xfer_resume reply", "the sender stayed blocked");
        return 0;
    }
    (void) pthread_join(ts, NULL);
    snprintf(detail, sizeof detail, "send rc=%d yields=%d by-budget=%d", snd.rc,
             yields, budgets);
    printf("xfer-resume: reply bytes=%d us=%lld %s\n", XR_BYTES, us, detail);
    check(snd.rc == 0 && memcmp(xr_back, xr_out, XR_BYTES) == 0,
          "xfer_resume reply bytes arrive", detail);
    (void) __connect_detach((uint64_t) snd.coid);
    (void) channel_destroy(chid);
    (void) munmap(xr_out, 3 * (size_t) XR_BYTES);
    return 0;
}
