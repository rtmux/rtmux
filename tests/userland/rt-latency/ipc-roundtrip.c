/*
 * ipc-roundtrip - native MsgSend/Receive/Reply round-trip WCET baseline.
 *
 * The native IPC round trip is far shorter than a scheduler tick, so the
 * software CLOCK_MONOTONIC (a per-tick counter) cannot see it. This probe reads
 * the RISC-V time counter (rdtime) directly instead, which the kernel now
 * exposes to U-mode via scounteren. It measures a genuinely cross-process round
 * trip so the address-space switch and copy the short-message path pays are on
 * the measured path.
 *
 * The parent owns the channel and runs a bare echo server; it spawns a child
 * that connects back and runs the timed msg_send loop. Roles are split so the
 * parent can pass its own pid and chid to the child by argument, which avoids
 * any channel-id rendezvous guesswork. The child is the measurer: for each
 * payload class it warms up, then times min/avg/max/p99 of the send-to-reply
 * round trip (rdtime ticks converted to ns via the syspage timebase rate) and
 * prints one line per class. The child always sends a terminal message so the
 * echo server returns cleanly: a stop opcode on success, a fail opcode on
 * error, so the parent reports the child's outcome without needing to reap it.
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
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>
#include <sys/syspage.h>

extern struct syspage_entry *_syspage_ptr;

#define SELF_PATH "/bin/ipc-roundtrip"
#define DEFAULT_ITERS 500
#define WARMUP 64
#define CAP_ITERS 8
#define CAP_WARMUP 2
#define SERVER_PRIORITY 61
#define CLIENT_PRIORITY 60
#define MAX_PAYLOAD 4096

/* First payload byte is an opcode. The echo server echoes OP_ECHO messages and
 * returns on a terminal opcode: OP_STOP for a clean finish, OP_FAIL if the
 * client hit an error, so the parent reports the child's outcome without a
 * wait.
 */
#define OP_ECHO 0x00
#define OP_STOP 0xEE
#define OP_FAIL 0xEF

struct payload_class {
    const char *name;
    unsigned bytes;
    unsigned caps;
};

static const struct payload_class classes[] = {
    {.name = "empty", .bytes = 0, .caps = 0},
    {.name = "small", .bytes = 128, .caps = 0},
    {.name = "page", .bytes = MAX_PAYLOAD, .caps = 0},
    {.name = "cap1", .bytes = sizeof(struct _io_native_hdr), .caps = 1},
    {.name = "cap2", .bytes = sizeof(struct _io_native_hdr), .caps = 2},
    {.name = "cap3", .bytes = sizeof(struct _io_native_hdr), .caps = 3},
    {.name = "cap4", .bytes = sizeof(struct _io_native_hdr), .caps = 4},
};
#define NUM_CLASSES (sizeof classes / sizeof classes[0])

/* Read the time counter (rdtime), not rdcycle, for two reasons. First, on a
 * multi-hart run the measuring thread can migrate, and rdcycle is a per-hart
 * counter whose values across harts are unrelated, which would corrupt a delta
 * taken around a blocking call; the time counter is a single mtime source
 * shared by all harts. Second, the syspage cycles_per_sec used for the ns
 * conversion is the mtime rate on this target, which matches rdtime but not
 * rdcycle (rdcycle runs far faster here). A real board whose CPU cycle rate
 * differs from its timebase would need the timebase frequency here instead of
 * cycles_per_sec.
 */
static inline uint64_t read_time(void)
{
    uint64_t t;

#if defined(__aarch64__)
    __asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(t));
#else
    __asm__ volatile("rdtime %0" : "=r"(t));
#endif
    return t;
}

/* Convert a counter delta to nanoseconds without a 128-bit divide (the
 * freestanding userland does not link libgcc's __udivti3). Splitting into whole
 * seconds plus remainder keeps every 64-bit multiply in range for any realistic
 * counter rate, so a long stalled sample cannot wrap the arithmetic.
 */
static uint64_t ticks_to_ns(uint64_t ticks, uint64_t rate)
{
    return (ticks / rate) * 1000000000ULL +
           (ticks % rate) * 1000000000ULL / rate;
}

static uint64_t cycles_per_sec(void)
{
    if (_syspage_ptr && _syspage_ptr->qtime.entry_off)
        return SYSPAGE_ENTRY(qtime)->cycles_per_sec;
    return 0;
}

static int raise_to_rt(int priority)
{
    struct sched_param sp = {.sched_priority = priority};

    if (__sched_set(0, 0, SCHED_FIFO, &sp) == -1) {
        fprintf(stderr, "ipc-roundtrip: __sched_set(%d): %s\n", priority,
                strerror(errno));
        return -1;
    }
    return 0;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t av = *(const uint64_t *) a;
    uint64_t bv = *(const uint64_t *) b;

    return (av > bv) - (av < bv);
}

/* p99 index into a sorted [0, samples) array; samples > 0 is guaranteed by the
 * caller. size_t math avoids the signed overflow that bites past ~21 M samples.
 */
static size_t p99_index(size_t samples)
{
    return ((samples - 1) * 99) / 100;
}

/* Parent: a bare echo server. Receives on chid, replies the same bytes back,
 * and returns once the stop opcode arrives. No allocation on the reply path.
 */
static int run_server(int chid)
{
    static char buf[MAX_PAYLOAD];

    if (raise_to_rt(SERVER_PRIORITY) != 0)
        return -1;

    for (;;) {
        struct _msg_info info;
        int rcvid = msg_receive(chid, buf, sizeof buf, &info);
        unsigned len;

        if (rcvid == -1) {
            fprintf(stderr, "ipc-roundtrip: server msg_receive: %s\n",
                    strerror(errno));
            return -1;
        }
        len = info.srcmsglen;
        if (len > sizeof buf)
            len = sizeof buf;
        if (len >= sizeof(struct _io_native_hdr)) {
            struct _io_native_hdr *hdr = (struct _io_native_hdr *) (void *) buf;

            if (hdr->msg.type == _IO_MSG &&
                hdr->msg.protocol == _IOMSG_RTMUX_NATIVE &&
                hdr->version == _IO_NATIVE_VERSION && hdr->nxfers > 0)
                len = 1;
        }
        if (msg_reply(rcvid, 0, buf, (int) len) == -1) {
            fprintf(stderr, "ipc-roundtrip: server msg_reply: %s\n",
                    strerror(errno));
            return -1;
        }
        if (len >= 1 && (unsigned char) buf[0] == OP_STOP)
            return 0;
        if (len >= 1 && (unsigned char) buf[0] == OP_FAIL)
            return 1;
    }
}

/* Client: connect to the parent's channel, retrying briefly in case the send
 * races the parent reaching its receive. The channel already exists (the parent
 * creates it before spawning us), so this normally succeeds on the first try.
 */
static int connect_server(int ppid, int chid)
{
    struct timespec backoff = {.tv_sec = 0, .tv_nsec = 100000}; /* 100 us */

    for (int tries = 0; tries < 1000; tries++) {
        int coid = connect_attach((uint32_t) 0, ppid, chid, RTMUX_SIDE_CHANNEL,
                                  0);
        if (coid != -1)
            return coid;
        if (errno != ESRCH && errno != ENOENT && errno != EAGAIN) {
            fprintf(stderr, "ipc-roundtrip: connect_attach: %s\n",
                    strerror(errno));
            return -1;
        }
        (void) nanosleep(&backoff, NULL);
    }
    fprintf(stderr, "ipc-roundtrip: connect_attach timed out\n");
    return -1;
}

static void report(const struct payload_class *cls, int iters,
                   uint64_t *samples, uint64_t cps)
{
    uint64_t min_c, max_c, p99_c, total = 0;

    /* Sorting first gives min and max for free at the ends, so the only pass
     * left to make is the sum, which is order-independent.
     */
    qsort(samples, (size_t) iters, sizeof *samples, cmp_u64);
    for (int i = 0; i < iters; i++)
        total += samples[i];
    min_c = samples[0];
    max_c = samples[iters - 1];
    p99_c = samples[p99_index((size_t) iters)];

    if (cps) {
        printf("ipc-roundtrip: class=%s bytes=%u iterations=%d min_ns=%llu "
               "avg_ns=%llu p99_ns=%llu max_ns=%llu caps=%u\n",
               cls->name, cls->bytes, iters,
               (unsigned long long) ticks_to_ns(min_c, cps),
               (unsigned long long) ticks_to_ns(total / (unsigned) iters, cps),
               (unsigned long long) ticks_to_ns(p99_c, cps),
               (unsigned long long) ticks_to_ns(max_c, cps), cls->caps);
    } else {
        printf("ipc-roundtrip: class=%s bytes=%u iterations=%d min_cyc=%llu "
               "avg_cyc=%llu p99_cyc=%llu max_cyc=%llu caps=%u "
               "(no cycle rate)\n",
               cls->name, cls->bytes, iters, (unsigned long long) min_c,
               (unsigned long long) (total / (unsigned) iters),
               (unsigned long long) p99_c, (unsigned long long) max_c,
               cls->caps);
    }
}

static int measure_class(int coid, const struct payload_class *cls, int iters,
                         uint64_t *samples, uint64_t cps, uint64_t cap)
{
    static char sbuf[MAX_PAYLOAD];
    static char rbuf[MAX_PAYLOAD];
    int warmup = cls->caps ? CAP_WARMUP : WARMUP;
    int samples_len = cls->caps && iters > CAP_ITERS ? CAP_ITERS : iters;
    unsigned reply_bytes = cls->caps ? 1U : cls->bytes;

    memset(sbuf, 0xA5, sizeof sbuf);
    if (cls->caps) {
        struct _io_native_hdr *hdr = (struct _io_native_hdr *) (void *) sbuf;

        memset(hdr, 0, sizeof(*hdr));
        hdr->msg.type = _IO_MSG;
        hdr->msg.combine_len = sizeof(*hdr);
        hdr->msg.protocol = _IOMSG_RTMUX_NATIVE;
        hdr->version = _IO_NATIVE_VERSION;
        hdr->nxfers = (uint16_t) cls->caps;
        for (unsigned i = 0; i < cls->caps; i++) {
            hdr->xfers[i].id = cap;
            hdr->xfers[i].rights = RTMUX_CAP_ENDPOINT_SEND;
        }
    } else if (cls->bytes) {
        sbuf[0] = OP_ECHO;
    }

    for (int i = 0; i < warmup + samples_len; i++) {
        uint64_t t0 = read_time();

        if (msg_send(coid, sbuf, cls->bytes, rbuf, reply_bytes) == -1) {
            fprintf(stderr, "ipc-roundtrip: msg_send(%s): %s\n", cls->name,
                    strerror(errno));
            return -1;
        }
        if (i >= warmup)
            samples[i - warmup] = read_time() - t0;

        if (!cls->caps && i == 0 && memcmp(sbuf, rbuf, cls->bytes) != 0) {
            fprintf(stderr, "ipc-roundtrip: echo mismatch on %s class\n",
                    cls->name);
            return -1;
        }
    }
    report(cls, samples_len, samples, cps);
    return 0;
}

static int measure_all(int coid, int iters, uint64_t cps, uint64_t cap)
{
    uint64_t *samples;

    if (raise_to_rt(CLIENT_PRIORITY) != 0)
        return -1;
    samples = calloc((size_t) iters, sizeof *samples);
    if (!samples) {
        fprintf(stderr, "ipc-roundtrip: calloc: %s\n", strerror(errno));
        return -1;
    }
    for (unsigned c = 0; c < NUM_CLASSES; c++) {
        if (classes[c].caps && cap == 0) {
            fprintf(stderr, "ipc-roundtrip: endpoint cap unavailable\n");
            free(samples);
            return -1;
        }
        if (measure_class(coid, &classes[c], iters, samples, cps, cap) != 0) {
            free(samples);
            return -1;
        }
    }
    free(samples);
    return 0;
}

static int run_client(int ppid, int chid, int iters)
{
    char term[1], term_reply[1];
    struct _server_info info;
    int coid, rc;

    coid = connect_server(ppid, chid);
    if (coid == -1)
        return -1;  /* No connection to signal the parent; it times out. */

    memset(&info, 0, sizeof(info));
    if (connect_server_info(0, (uint64_t) coid, &info) != coid ||
        info.endpoint_cap == 0) {
        fprintf(stderr, "ipc-roundtrip: connect_server_info: %s\n",
                strerror(errno));
        return -1;
    }

    rc = measure_all(coid, iters, cycles_per_sec(), info.endpoint_cap);

    /* Always release the echo server with a terminal opcode carrying the
     * outcome, so the parent returns the child's result instead of blocking in
     * msg_receive forever. A matching reply buffer keeps the final reply the
     * same shape as the measured ones, so no zero-capacity reply edge case.
     */
    term[0] = (char) (rc == 0 ? OP_STOP : OP_FAIL);
    if (msg_send(coid, term, sizeof term, term_reply, sizeof term_reply) == -1) {
        fprintf(stderr, "ipc-roundtrip: terminal msg_send: %s\n",
                strerror(errno));
        return -1;
    }
    return rc;
}

int main(int argc, char **argv)
{
    int iters = DEFAULT_ITERS;
    int chid, ppid;
    pid_t child;
    char pid_arg[16], chid_arg[16], iters_arg[16];
    char *child_argv[6];

    if (argc >= 5 && !strcmp(argv[1], "--client")) {
        ppid = atoi(argv[2]);
        chid = atoi(argv[3]);
        iters = atoi(argv[4]);
        if (ppid <= 0 || chid <= 0 || iters <= 0) {
            fprintf(stderr, "ipc-roundtrip: bad client args\n");
            return EXIT_FAILURE;
        }
        return run_client(ppid, chid, iters) == 0 ? EXIT_SUCCESS
                                                  : EXIT_FAILURE;
    }

    if (argc > 1)
        iters = atoi(argv[1]);
    if (iters <= 0) {
        fprintf(stderr, "usage: ipc-roundtrip [iterations]\n");
        return EXIT_FAILURE;
    }

    chid = channel_create(0);
    if (chid == -1) {
        fprintf(stderr, "ipc-roundtrip: channel_create: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    snprintf(pid_arg, sizeof pid_arg, "%d", getpid());
    snprintf(chid_arg, sizeof chid_arg, "%d", chid);
    snprintf(iters_arg, sizeof iters_arg, "%d", iters);
    child_argv[0] = (char *) SELF_PATH;
    child_argv[1] = (char *) "--client";
    child_argv[2] = pid_arg;
    child_argv[3] = chid_arg;
    child_argv[4] = iters_arg;
    child_argv[5] = NULL;

    if (posix_spawn(&child, SELF_PATH, NULL, NULL, child_argv, NULL) != EOK) {
        fprintf(stderr, "ipc-roundtrip: posix_spawn: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }

    /* The echo server only returns once the client has sent its stop message,
     * which the client does only after both classes were measured cleanly. So a
     * clean server return already proves client success; the orphaned child is
     * reaped by init. We deliberately do not waitpid here: the measurer child
     * runs at a lower fixed priority than this server, and reaping it back adds
     * no signal the stop handshake has not already given.
     */
    (void) child;
    return run_server(chid) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
