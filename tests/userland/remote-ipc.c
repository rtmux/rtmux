/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * remote-ipc — the kernel's remote IPC paths, from a process that registers
 * as the remote IPC service.
 *
 * No such service exists in the system, and a started process cannot become
 * one: it takes the IPC capability. The debug fault-injection call grants it
 * to this process (/bin/inject grant-ipc-cap, CONFIG_DEBUG_INJECT kernels
 * only), which then walks the paths a service reaches:
 *
 * - registering the service channel, once;
 * - a COF_NETCON connection, a send on it (which acts for a virtual thread,
 *   and none exists), and an rcvid that names it;
 * - a receive of the service's own send on its service channel, whose
 *   receive info is filled by the remote branch;
 * - all of it again after the service channel is destroyed under the
 *   connections: every call must fail, and none may fault.
 *
 * This runs the paths that can be reached. It is not a race test: the rcvid
 * calls stop at the missing remote credentials and the send at the missing
 * virtual thread, before the kernel's reads of the connection's channel, and
 * nothing here overlaps a destroy.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>

#include "test.h"

struct ri_send {
    int coid;
    atomic_int done;
};

static void *ri_sender(void *raw)
{
    struct ri_send *s = raw;
    char req = 'r', rep = 0;

    (void) msg_send(s->coid, &req, 1, &rep, 1);
    atomic_store(&s->done, 1);
    return NULL;
}

/* A send on the NETCON connection, as the service makes for virtual thread 1:
 * its reply arguments are the thread's number and its description.
 */
static int ri_netcon_send(int ncoid)
{
    struct _vtid_info vt;
    char req = 'n';
    iov_t siov;

    memset(&vt, 0, sizeof vt);
    SETIOV(&siov, &req, 1);
    return msg_sendv(ncoid, &siov, 1, (iov_t *) (void *) &vt, 1) == -1 ? errno
                                                                       : 0;
}

/* What the calls that take an rcvid make of one that names the NETCON
 * connection: errno of msg_info, msg_error and msg_reply, in that order.
 */
static void ri_rcvid_calls(int rcvid, int err[3])
{
    struct _msg_info info;
    char rep = 0;

    err[0] = msg_info(rcvid, &info) == -1 ? errno : 0;
    err[1] = msg_error(rcvid, EINTR) == -1 ? errno : 0;
    err[2] = msg_reply(rcvid, 0, &rep, 1) == -1 ? errno : 0;
}

int test_remote_ipc(void)
{
    struct ri_send snd;
    struct _server_info sinfo;
    struct _msg_info info;
    pthread_t ts;
    uint64_t ns = 2000000000ULL;
    char pid[16], detail[160], buf = 0, rep = 'k';
    int svc, again, ncoid, coid, rcvid, nrcvid, err[3], serr, plain;

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    snprintf(pid, sizeof pid, "%d", (int) getpid());

    svc = channel_create(RTMUX_FLG_CHF_NET_MSG);
    serr = errno;
    snprintf(detail, sizeof detail, "chid=%d errno=%d", svc,
             svc == -1 ? serr : 0);
    check(svc == -1 && serr == EPERM,
          "remote_ipc refused without the capability", detail);
    if (svc != -1)
        (void) channel_destroy(svc);
    if (inject_run("grant-ipc-cap", pid, NULL) != 0) {
        test_fail("remote_ipc", "inject refused to grant the capability");
        return 0;
    }

    svc = channel_create(RTMUX_FLG_CHF_NET_MSG);
    again = channel_create(RTMUX_FLG_CHF_NET_MSG);
    serr = errno;
    snprintf(detail, sizeof detail, "service chid=%d, second chid=%d errno=%d",
             svc, again, again == -1 ? serr : 0);
    check(svc != -1 && again == -1 && serr == EAGAIN,
          "remote_ipc service registers once", detail);
    if (again != -1)
        (void) channel_destroy(again);
    if (svc == -1)
        return 0;

    /* The service's own connection for remote clients, and an rcvid that
     * names it (thread 0 behind its slot in the service's table).
     */
    ncoid = connect_attach(0, getpid(), svc, RTMUX_SIDE_CHANNEL,
                           RTMUX_FLG_COF_NETCON);
    serr = errno;
    memset(&sinfo, 0, sizeof sinfo);
    again = ncoid == -1 ? -1 : connect_server_info(0, (uint64_t) ncoid, &sinfo);
    if (ncoid != -1 && again == -1)
        serr = errno;
    if (ncoid == -1 || again != ncoid) {
        /* errno is of the call that failed; a server info that names some
         * other connection set none.
         */
        test_fail("remote_ipc NETCON connection",
                  ncoid != -1 && again != -1
                      ? "server info names another connection"
                      : strerror(serr));
        if (ncoid != -1)
            (void) __connect_detach((uint64_t) ncoid);
        (void) channel_destroy(svc);
        return 0;
    }
    nrcvid =
        sinfo.scoid & (int) ~RTMUX_SIDE_CHANNEL; /* thread 0: the slot alone */
    serr = ri_netcon_send(ncoid);
    ri_rcvid_calls(nrcvid, err);
    snprintf(detail, sizeof detail,
             "send errno=%d; info=%d error=%d reply=%d (rcvid %#x)", serr,
             err[0], err[1], err[2], nrcvid);
    check(serr == EINVAL,
          "remote_ipc send for a virtual thread that does not exist", detail);
    check(err[0] == EBADF && err[1] == EBADF && err[2] == EBADF,
          "remote_ipc rcvid of a connection with no remote credentials",
          detail);

    /* The service sends on its own service channel: the receive info is
     * filled by the branch for the service's own requests.
     */
    coid = connect_attach(0, getpid(), svc, RTMUX_SIDE_CHANNEL, 0);
    snd = (struct ri_send) {.coid = coid};
    if (coid == -1 || pthread_create(&ts, NULL, ri_sender, &snd) != 0) {
        test_fail("remote_ipc sender", strerror(errno));
        if (coid != -1)
            (void) __connect_detach((uint64_t) coid);
        (void) __connect_detach((uint64_t) ncoid);
        (void) channel_destroy(svc);
        return 0;
    }
    memset(&info, 0, sizeof info);
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL, &ns,
                           NULL);
    rcvid = msg_receive(svc, &buf, 1, &info);
    snprintf(detail, sizeof detail, "rcvid=%d byte=%d info pid=%d chid=%d",
             rcvid, buf, (int) info.pid, info.chid);
    check(rcvid > 0 && buf == 'r' && info.pid == getpid() && info.chid == svc,
          "remote_ipc receive info of the service's own send", detail);
    if (rcvid > 0)
        (void) msg_reply(rcvid, 0, &rep, 1);
    for (int i = 0; i < 2000 && !atomic_load(&snd.done); i++)
        sleep_ms(1);
    if (!atomic_load(&snd.done))
        test_fail("remote_ipc sender", "the send did not return");

    /* The service channel goes, under both connections. The registration
     * goes with it, and every call above fails where it used to succeed or
     * fail: none may fault on the channel that is no longer there.
     */
    (void) channel_destroy(svc);
    (void) pthread_join(ts, NULL); /* the destroy ends a sender still waiting */
    serr = ri_netcon_send(ncoid);
    ri_rcvid_calls(nrcvid, err);
    plain = msg_send(coid, &buf, 1, &rep, 1);
    snprintf(
        detail, sizeof detail,
        "send errno=%d; info=%d error=%d reply=%d; plain send rc=%d errno=%d",
        serr, err[0], err[1], err[2], plain, plain == -1 ? errno : 0);
    check(serr != 0 && err[0] != 0 && err[1] != 0 && err[2] != 0 && plain == -1,
          "remote_ipc calls fail on a destroyed service channel", detail);
    svc = channel_create(RTMUX_FLG_CHF_NET_MSG);
    serr = errno;
    snprintf(detail, sizeof detail, "chid=%d errno=%d", svc,
             svc == -1 ? serr : 0);
    check(svc != -1,
          "remote_ipc service registers again after its channel went", detail);
    if (svc != -1)
        (void) channel_destroy(svc);
    (void) __connect_detach((uint64_t) ncoid);
    (void) __connect_detach((uint64_t) coid);
    return 0;
}
