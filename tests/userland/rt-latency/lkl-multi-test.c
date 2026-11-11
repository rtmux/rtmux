/*
 * lkl-multi-test - cross-subsystem data flow across two live LKL domains.
 *
 * Runs inside the lkl-multi scenario after both lkl-netd (/net) and lkl-fsd
 * (/fs/root) are serving. One client fetches a fixed payload from the host over
 * TCP through the network domain, writes it into a file on the filesystem
 * domain, reads it back, and checks the round-trip bytes against the pattern
 * the host sent. This exercises data crossing net -> fs through two separate
 * Linux instances from a single client, which the per-domain smokes do not.
 *
 * The host side is a raw-TCP payload server (scripts/qemu-regress.sh, port
 * 8003) that sends PAYLOAD_LEN bytes of a 0..255 ramp and closes -- no HTTP, so
 * the guest just reads to EOF. The ramp is regenerated here as the "host
 * source" oracle; a mismatch fails closed. Prints one PASS/FAIL line and exits
 * non-zero on any failure so the init chain gates on it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define HOST "10.0.2.2"
#define PORT "8003"
#define PAYLOAD_LEN 8192
#define DL_PATH "/fs/root/dl.bin"
/* Max consecutive would-blocks to ride out on one socket read before calling it
 * a stall (1 ms apart). Generous for a healthy run; the stress negative control
 * (a starved timer path) blows through it and fails, which is the point.
 */
#define MAX_EAGAIN 3000

/* read() that rides out transient EAGAINs from the non-blocking LKL socket:
 * under sustained SMP load the netd worker can momentarily lag the reader, so a
 * would-block is a yield-and-retry, not an error. Bounded so a real stall still
 * fails. On the filesystem fd EAGAIN never occurs, so this is a plain read.
 */
static ssize_t read_retry(int fd, void *buf, size_t n)
{
    for (int idle = 0; idle < MAX_EAGAIN; idle++) {
        ssize_t r = read(fd, buf, n);

        if (r >= 0 || errno != EAGAIN)
            return r;
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 1000000}, NULL);
    }
    errno = EAGAIN;
    return -1;
}

/* The host payload server sends this exact pattern; keep the two in sync. */
static void fill_expected(unsigned char *buf, size_t len)
{
    for (size_t i = 0; i < len; i++)
        buf[i] = (unsigned char) (i & 0xff);
}

/* Read up to cap bytes. If the source still has data once cap is reached, set
 * *out_len to cap + 1 so the caller's exact-length check rejects an overlong
 * payload instead of silently truncating it to a false match.
 */
static int read_all(int fd, unsigned char *buf, size_t cap, size_t *out_len)
{
    size_t len = 0;

    while (len < cap) {
        ssize_t n = read_retry(fd, buf + len, cap - len);

        if (n < 0)
            return -1;
        if (n == 0)
            break; /* peer closed: end of payload */
        len += (size_t) n;
    }
    if (len == cap) {
        unsigned char extra;
        ssize_t n = read_retry(fd, &extra, 1);

        if (n < 0)
            return -1;
        if (n > 0)
            len = cap + 1; /* overlong: fails the exact-length check */
    }
    *out_len = len;
    return 0;
}

static int write_all(int fd, const unsigned char *buf, size_t len)
{
    for (size_t off = 0; off < len;) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n <= 0)
            return -1;
        off += (size_t) n;
    }
    return 0;
}

static void net_tcp_close(const char *id)
{
    char path[48];
    int fd;

    snprintf(path, sizeof(path), "/net/tcp/%s/ctl", id);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        return;
    (void) write(fd, "close\n", 6);
    close(fd);
}

/* Clone a TCP endpoint from lkl-netd, connect it to HOST:PORT, and return the
 * open data fd; stash the clone id so the caller can close it. Mirrors the
 * /net/tcp/clone -> ctl "connect" -> data handshake the toolbox net tools use.
 * On any failure after the clone id is allocated, close the clone so a failed
 * connect does not leak one of lkl-netd's scarce socket slots.
 */
static int net_tcp_connect(char *id, size_t idsz)
{
    char path[48];
    char cmd[64];
    int fd;
    ssize_t n;

    fd = open("/net/tcp/clone", O_RDONLY);
    if (fd < 0)
        return -1;
    n = read(fd, id, idsz - 1);
    close(fd);
    if (n <= 0)
        return -1;
    id[n] = '\0';
    id[strcspn(id, "\r\n")] = '\0'; /* trim the trailing newline lkl-netd emits */
    if (id[0] == '\0' || strchr(id, '/'))
        return -1;

    snprintf(path, sizeof(path), "/net/tcp/%s/ctl", id);
    fd = open(path, O_WRONLY);
    if (fd < 0)
        goto fail;
    snprintf(cmd, sizeof(cmd), "connect %s %s\n", HOST, PORT);
    if (write(fd, cmd, strlen(cmd)) != (ssize_t) strlen(cmd)) {
        close(fd);
        goto fail;
    }
    close(fd);

    snprintf(path, sizeof(path), "/net/tcp/%s/data", id);
    fd = open(path, O_RDWR);
    if (fd < 0)
        goto fail;
    return fd;

fail:
    net_tcp_close(id);
    return -1;
}

static unsigned char rx[PAYLOAD_LEN];
static unsigned char back[PAYLOAD_LEN];
static unsigned char expected[PAYLOAD_LEN];

/* One net->fs->readback cycle: pull the payload from the host through lkl-netd,
 * write it to lkl-fsd, read it straight back, and check both directions against
 * the host source.
 *
 * Returns 0 on success, -1 on any failure (fail-closed).
 */
static int run_cycle(int durable)
{
    char id[16];
    int fd;
    size_t rx_len = 0;
    size_t back_len = 0;

    /* net domain: pull the payload from the host over TCP. */
    fd = net_tcp_connect(id, sizeof(id));
    if (fd < 0) {
        fprintf(stderr, "lkl-multi-test: connect %s:%s: %s\n", HOST, PORT,
                strerror(errno));
        return -1;
    }
    if (read_all(fd, rx, sizeof(rx), &rx_len) != 0) {
        fprintf(stderr, "lkl-multi-test: recv: %s\n", strerror(errno));
        close(fd);
        net_tcp_close(id);
        return -1;
    }
    close(fd);
    net_tcp_close(id);
    if (rx_len != PAYLOAD_LEN) {
        fprintf(stderr, "lkl-multi-test: recv len %d != %d\n", (int) rx_len,
                PAYLOAD_LEN);
        return -1;
    }
    if (memcmp(rx, expected, PAYLOAD_LEN) != 0) {
        fprintf(stderr, "lkl-multi-test: received payload != host source\n");
        return -1;
    }

    /* fs domain: write the payload, then read it straight back. */
    fd = open(DL_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write_all(fd, rx, PAYLOAD_LEN) != 0) {
        fprintf(stderr, "lkl-multi-test: write %s: %s\n", DL_PATH,
                strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    if (durable && fsync(fd) != 0) {
        fprintf(stderr, "lkl-multi-test: fsync %s: %s\n", DL_PATH,
                strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    fd = open(DL_PATH, O_RDONLY);
    if (fd < 0 || read_all(fd, back, sizeof(back), &back_len) != 0) {
        fprintf(stderr, "lkl-multi-test: read %s: %s\n", DL_PATH,
                strerror(errno));
        if (fd >= 0)
            close(fd);
        return -1;
    }
    close(fd);

    if (back_len != PAYLOAD_LEN || memcmp(back, expected, PAYLOAD_LEN) != 0) {
        fprintf(stderr, "lkl-multi-test: fs read-back != host source\n");
        return -1;
    }
    return 0;
}

/* Optional argv[1] is a repeat count (default 1). The stress mode passes a
 * large count to sustain net+fs load across both domains and pound the shared
 * timer path; one iteration is the plain lkl-multi cross-subsystem check.
 */
int main(int argc, char **argv)
{
    int iters = 1;

    if (argc > 1) {
        /* Clamp the accumulator so a long digit string cannot overflow int
         * (signed overflow is UB); garbage/negative input matches no leading
         * digit and falls through to 1.
         */
        long v = 0;

        for (const char *p = argv[1]; *p >= '0' && *p <= '9' && v < 1000000; p++)
            v = v * 10 + (*p - '0');
        iters = v < 1 ? 1 : (int) v;
    }

    fill_expected(expected, PAYLOAD_LEN);
    for (int i = 0; i < iters; i++)
        if (run_cycle(i == iters - 1) != 0) {
            fprintf(stderr, "lkl-multi-test: cycle %d of %d failed\n", i + 1,
                    iters);
            return 1;
        }

    printf("lkl-multi-test: net->fs->readback ok len=%d iters=%d\n", PAYLOAD_LEN,
           iters);
    return 0;
}
