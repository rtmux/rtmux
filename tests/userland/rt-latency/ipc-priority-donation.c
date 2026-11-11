/*
 * ipc-priority-donation - verify IPC priority donation and transitivity.
 *
 * Section 18 core-path realtime characterization. When a higher-priority client
 * sends to a lower-priority server, the kernel donates the client's priority to
 * the receiving server thread for the duration of the request, so the server
 * runs at the client's priority instead of its low base. This regression proves
 * that from user space without reading a foreign thread's priority (which RTMux
 * does not allow): the server reads its OWN sched_curpriority while processing
 * a request and replies it, and the client asserts it matches the client's
 * priority.
 *
 * Donation is checked at two priorities: one client thread sends first at a
 * high priority, then lowers itself and sends again. Each request is serviced
 * at that request's own sender priority, so a stale higher priority does not
 * leak across requests (the second request observing the lower priority, not
 * the earlier higher one). This proves per-request donation tracks the current
 * sender, not that the server thread is restored to its base between requests:
 * the receive path overwrites the effective priority with each new sender, so a
 * stale value cannot survive, but this test does not separately observe the
 * base.
 *
 * A three-process chain (client -> server A -> server B) additionally checks
 * that donation is transitive: server A, itself boosted to the client's
 * priority, sends to server B while blocked on it, so B must also observe the
 * client's priority. A relays B's observed priority back to the client.
 *
 * The client reports the verdict to the top server as a terminal opcode so the
 * top-level server process exit status is the test result.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sched.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/rtmux-core.h>

#define SELF_PATH "/bin/ipc-priority-donation"

/* All SCHED_FIFO. Higher number is higher priority. The server base sits below
 * both client priorities so donation is a real boost; the two client priorities
 * are distinct so a stale (unrestored) donation is detectable.
 */
#define SERVER_A_BASE 10
#define SERVER_B_BASE 12
#define CLIENT_HIGH 40
#define CLIENT_MID 25

/* B's rendezvous channel id. libc creates each process's system channel (chid
 * 1) at startup, so the first user channel_create returns 2. Server B asserts
 * this so a layout change is a loud failure, not a silent hang.
 */
#define SERVER_B_CHID 2

/* A request carries no data; the reply carries the server's sched_curpriority
 * observed while it processed the request. The terminal opcode ends the server
 * and carries the client verdict: STOP on pass, FAIL on any mismatch.
 */
#define OP_REQ 0x00
#define OP_STOP 0xEE
#define OP_FAIL 0xEF

static int set_fifo(int priority)
{
    struct sched_param sp = {.sched_priority = priority};

    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1) {
        fprintf(stderr, "donation: __sched_set(%d): %s\n", priority,
                strerror(errno));
        return -1;
    }
    return 0;
}

static int my_curpriority(void)
{
    struct sched_param sp;

    if (sched_getparam(0, &sp) == -1) {
        fprintf(stderr, "donation: sched_getparam: %s\n", strerror(errno));
        return -1;
    }
    return sp.sched_curpriority;
}

static int connect_retry(int pid, int chid)
{
    struct timespec backoff = {.tv_sec = 0, .tv_nsec = 100000};

    for (int tries = 0; tries < 1000; tries++) {
        int coid = connect_attach((uint32_t) 0, pid, chid, RTMUX_SIDE_CHANNEL, 0);
        if (coid != -1)
            return coid;
        if (errno != ESRCH && errno != ENOENT && errno != EAGAIN) {
            fprintf(stderr, "donation: connect_attach: %s\n", strerror(errno));
            return -1;
        }
        (void) nanosleep(&backoff, NULL);
    }
    fprintf(stderr, "donation: connect_attach timed out\n");
    return -1;
}

/* Leaf server B: base below the clients. On each request, reply its own donated
 * priority. STOP ends it.
 */
static int run_server_b(void)
{
    int chid = channel_create(0);

    if (chid == -1) {
        fprintf(stderr, "donation: B channel_create: %s\n", strerror(errno));
        return -1;
    }
    if (chid != SERVER_B_CHID) {
        fprintf(stderr, "donation: B chid %d, expected %d\n", chid,
                SERVER_B_CHID);
        return -1;
    }
    if (set_fifo(SERVER_B_BASE) != 0)
        return -1;
    for (;;) {
        char buf[1];
        int rcvid = msg_receive(chid, buf, sizeof buf, NULL);
        int cur;
        char r;

        if (rcvid == -1) {
            fprintf(stderr, "donation: B msg_receive: %s\n", strerror(errno));
            return -1;
        }
        if ((unsigned char) buf[0] == OP_STOP) {
            char z = 0;
            (void) msg_reply(rcvid, 0, &z, 1);
            return 0;
        }
        cur = my_curpriority();
        if (cur == -1)
            return -1;
        r = (char) cur;
        if (msg_reply(rcvid, 0, &r, 1) == -1) {
            fprintf(stderr, "donation: B msg_reply: %s\n", strerror(errno));
            return -1;
        }
    }
}

/* Middle server A: base below the clients. On each request, record its own
 * donated priority, then forward to B while blocked on it so donation passes
 * through, and reply both A's and B's observed priority. The terminal opcode
 * ends A (and stops B) and carries the client verdict.
 *
 * Returns 0 on STOP, 1 on FAIL.
 */
static int run_server_a(int chid, int b_coid)
{
    if (set_fifo(SERVER_A_BASE) != 0)
        return -1;
    for (;;) {
        char buf[1];
        int rcvid = msg_receive(chid, buf, sizeof buf, NULL);
        int a_cur;
        char req = OP_REQ, b_reply = 0, r[2];

        if (rcvid == -1) {
            fprintf(stderr, "donation: A msg_receive: %s\n", strerror(errno));
            return -1;
        }
        if ((unsigned char) buf[0] == OP_STOP ||
            (unsigned char) buf[0] == OP_FAIL) {
            int verdict = ((unsigned char) buf[0] == OP_FAIL);
            char stop = OP_STOP, z = 0;

            (void) msg_send(b_coid, &stop, 1, &z, 1);
            (void) msg_reply(rcvid, 0, &z, 1);
            return verdict;
        }
        a_cur = my_curpriority();
        if (a_cur == -1)
            return -1;
        if (msg_send(b_coid, &req, 1, &b_reply, 1) == -1) {
            fprintf(stderr, "donation: A->B msg_send: %s\n", strerror(errno));
            return -1;
        }
        r[0] = (char) a_cur;
        r[1] = b_reply;
        if (msg_reply(rcvid, 0, r, sizeof r) == -1) {
            fprintf(stderr, "donation: A msg_reply: %s\n", strerror(errno));
            return -1;
        }
    }
}

/* Client: send at HIGH then MID and assert the donated priority observed by
 * both A and B matches the sender priority each time.
 */
static int run_client(int a_pid, int a_chid)
{
    int coid = connect_retry(a_pid, a_chid);
    int fail = 0;
    struct {
        const char *name;
        int prio;
    } cases[] = {{"high", CLIENT_HIGH}, {"mid", CLIENT_MID}};

    if (coid == -1)
        return -1;

    /* Once connected, every exit path funnels through the terminal send below
     * so server A is always released instead of blocking in msg_receive until
     * the harness timeout. An error before the terminal send counts as a
     * failure.
     */
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0] && !fail; i++) {
        char req = OP_REQ, reply[2] = {0, 0};
        int a_seen, b_seen;

        if (set_fifo(cases[i].prio) != 0) {
            fail = 1;
            break;
        }
        if (msg_send(coid, &req, 1, reply, sizeof reply) == -1) {
            fprintf(stderr, "donation: client msg_send: %s\n", strerror(errno));
            fail = 1;
            break;
        }
        a_seen = (unsigned char) reply[0];
        b_seen = (unsigned char) reply[1];
        printf("ipc-priority-donation: client=%s prio=%d A_donated=%d "
               "B_donated=%d\n",
               cases[i].name, cases[i].prio, a_seen, b_seen);
        if (a_seen != cases[i].prio) {
            fprintf(stderr, "donation: FAIL A saw %d expected %d (%s)\n",
                    a_seen, cases[i].prio, cases[i].name);
            fail = 1;
        }
        if (b_seen != cases[i].prio) {
            fprintf(stderr,
                    "donation: FAIL B transitive saw %d expected %d (%s)\n",
                    b_seen, cases[i].prio, cases[i].name);
            fail = 1;
        }
    }

    {
        char term = (char) (fail ? OP_FAIL : OP_STOP), z = 0;
        (void) msg_send(coid, &term, 1, &z, 1);
    }
    return fail ? -1 : 0;
}

int main(int argc, char **argv)
{
    int chid, b_coid;
    pid_t b_pid;
    char pid_arg[16], chid_arg[16];
    char *child_argv[5];

    if (argc >= 2 && !strcmp(argv[1], "--server-b"))
        return run_server_b() == 0 ? EXIT_SUCCESS : EXIT_FAILURE;

    if (argc >= 4 && !strcmp(argv[1], "--client")) {
        int pid = atoi(argv[2]), cid = atoi(argv[3]);

        if (pid <= 0 || cid <= 0) {
            fprintf(stderr, "donation: bad client args\n");
            return EXIT_FAILURE;
        }
        return run_client(pid, cid) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* Top level is server A. Spawn leaf server B and connect to it, then spawn
     * the client and run A's forwarding loop. A's exit status is the verdict.
     */
    child_argv[0] = (char *) SELF_PATH;
    child_argv[1] = (char *) "--server-b";
    child_argv[2] = NULL;
    if (posix_spawn(&b_pid, SELF_PATH, NULL, NULL, child_argv, NULL) != EOK) {
        fprintf(stderr, "donation: spawn B: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    b_coid = connect_retry(b_pid, SERVER_B_CHID);
    if (b_coid == -1)
        return EXIT_FAILURE;

    chid = channel_create(0);
    if (chid == -1) {
        fprintf(stderr, "donation: A channel_create: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    snprintf(pid_arg, sizeof pid_arg, "%d", getpid());
    snprintf(chid_arg, sizeof chid_arg, "%d", chid);
    child_argv[0] = (char *) SELF_PATH;
    child_argv[1] = (char *) "--client";
    child_argv[2] = pid_arg;
    child_argv[3] = chid_arg;
    child_argv[4] = NULL;
    if (posix_spawn(NULL, SELF_PATH, NULL, NULL, child_argv, NULL) != EOK) {
        fprintf(stderr, "donation: spawn client: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    return run_server_a(chid, b_coid) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
