/*
 * lkl-stale-handle-test - a handle to a dead LKL generation must fail closed.
 *
 * Capability decides authority (Standing Gate): a client holding an open handle
 * (coid) to an LKL domain server must not keep any authority once that server
 * dies. This runs against lkl-netd --serve-net-crash-on-ping: it opens a
 * writable control handle (/net/dns), confirms the handle works while the
 * server lives, faults the server, then keeps using the SAME handle and
 * requires the operation to fail -- deterministically and promptly, not hang
 * and not silently succeed. A write is used because it always round-trips to
 * the server (no client-side EOF caching), so a dead server surfaces as an IPC
 * error.
 *
 * Prints one PASS/FAIL line and exits non-zero on any failure.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DNS "/net/dns"
#define PING "/net/ping"
#define NS_CMD "nameserver 8.8.8.8\n"
#define POLL_MS 100
#define POLL_MAX 100 /* 10s: a dead-server write must surface well within this */

static void nap_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};

    nanosleep(&ts, NULL);
}

int main(void)
{
    int fd, ping_fd;

    /* Open a writable control handle to the live server and confirm it works.
     */
    fd = open(DNS, O_WRONLY);
    if (fd < 0) {
        fprintf(stderr, "lkl-stale-handle-test: open %s: %s\n", DNS,
                strerror(errno));
        return 1;
    }
    if (write(fd, NS_CMD, strlen(NS_CMD)) < 0) {
        fprintf(stderr, "lkl-stale-handle-test: baseline write: %s\n",
                strerror(errno));
        close(fd);
        return 1;
    }

    /* Fault the server: a crash-on-ping lkl-netd dies when a ping line is
     * written to /net/ping. The handle above is now bound to a dead generation.
     */
    ping_fd = open(PING, O_WRONLY);
    if (ping_fd >= 0) {
        (void) write(ping_fd, "10.0.2.2 1\n", 11);
        close(ping_fd);
    }

    /* The stale handle must fail closed. Poll: while the server is still
     * tearing down the write may transiently still land, but once the server is
     * gone the write has to return an error rather than block or succeed.
     */
    for (int i = 0; i < POLL_MAX; i++) {
        if (write(fd, NS_CMD, strlen(NS_CMD)) < 0) {
            printf("lkl-stale-handle-test: stale handle failed closed (errno=%d)\n",
                   errno);
            printf("lkl-stale-handle-test: PASS\n");
            close(fd);
            return 0;
        }
        nap_ms(POLL_MS);
    }

    fprintf(stderr,
            "lkl-stale-handle-test: FAIL stale handle still writable after crash\n");
    close(fd);
    return 1;
}
