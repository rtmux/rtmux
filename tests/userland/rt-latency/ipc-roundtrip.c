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
 * Before the timed run the parent also checks msg_readiov on a send part that
 * straddles a page boundary into a separately allocated frame: the physical
 * parts it returns must split at the boundary and name the frames mem_offset
 * reports, not run on past the first frame.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdatomic.h>
#include <sched.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/iomsg.h>
#include <sys/mman.h>
#include <sys/rtmux-core.h>
#include <sys/syspage.h>

#include "../outcome.h"

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

    __asm__ volatile("rdtime %0" : "=r"(t));
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

/* msg_readiov straddle check. A sender thread in this process sends a part
 * that starts STRADDLE_HEAD bytes before a page boundary, behind a pad part
 * that makes the send too long for the kernel's short-message buffer, and the
 * receiver asks for the straddling part as physical parts. Each physical part
 * must stay inside the frame that maps its first byte, so with two non-adjacent
 * frames there must be two parts. A short send first checks that msg_readiov
 * refuses a message that was buffered and so has no client iov to describe.
 */
#define STRADDLE_HEAD 8
#define STRADDLE_LEN 16
#define STRADDLE_PAD 512
#define STRADDLE_TRIES 8
#define STRADDLE_TIMEOUT_NS 5000000000ULL

struct straddle_ctx {
    int chid;
    char *src;
    int rc;
};

static void *straddle_sender(void *arg)
{
    static char pad[STRADDLE_PAD];
    struct straddle_ctx *ctx = arg;
    iov_t siov[2];
    int coid;

    ctx->rc = -1;
    coid = connect_attach((uint32_t) 0, 0, ctx->chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        fprintf(stderr, "ipc-roundtrip: straddle connect_attach: %s\n",
                strerror(errno));
        return NULL;
    }
    SETIOV(&siov[0], pad, sizeof pad);
    SETIOV(&siov[1], ctx->src, STRADDLE_LEN);
    if (msg_sendv(coid, &siov[1], 1, NULL, 0) == -1 ||
        msg_sendv(coid, siov, 2, NULL, 0) == -1)
        fprintf(stderr, "ipc-roundtrip: straddle msg_sendv: %s\n",
                strerror(errno));
    else
        ctx->rc = 0;
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

static int page_paddr(const void *va, off_t *pa)
{
    size_t contig;

    if (mem_offset(va, NOFD, 1, pa, &contig) == -1) {
        fprintf(stderr, "ipc-roundtrip: straddle mem_offset: %s\n",
                strerror(errno));
        return -1;
    }
    return 0;
}

static char *map_anon(void *addr, size_t len, int flags)
{
    char *p = mmap(addr, len, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON | flags, NOFD, 0);

    if (p == MAP_FAILED) {
        fprintf(stderr, "ipc-roundtrip: straddle mmap: %s\n", strerror(errno));
        return NULL;
    }
    p[0] = 1;
    return p;
}

/* Map two virtually adjacent pages whose frames are not physically adjacent.
 * Reserve two pages of address space and release them, then map the upper
 * page, a spacer page elsewhere, and the lower page, each as its own one-page
 * mapping touched as soon as it is made. An allocator that hands out frames in
 * address order, up or down, puts the spacer's frame between the other two, so
 * the upper page's frame cannot directly follow the lower page's. Frames reused
 * out of order can still line up, hence the retries. The spacer stays mapped
 * for the run, and so does a rejected pair, so a retry cannot get those frames
 * back; at most STRADDLE_TRIES pairs and spacers stay mapped. Physical
 * adjacency is a property of the allocator, not of the walk under test, so a
 * host where every try is adjacent skips the check. Return value: 0 with the
 * pair in *pairp, 1 if every try was adjacent, -1 on error.
 */
static int map_split_pair(size_t pg, char **pairp, off_t *pa0, off_t *pa1)
{
    for (int i = 0; i < STRADDLE_TRIES; i++) {
        char *pair = mmap(NULL, 2 * pg, PROT_NONE, MAP_PRIVATE | MAP_ANON,
                          NOFD, 0);

        if (pair == MAP_FAILED || munmap(pair, 2 * pg) == -1) {
            fprintf(stderr, "ipc-roundtrip: straddle reserve: %s\n",
                    strerror(errno));
            return -1;
        }
        if (!map_anon(pair + pg, pg, MAP_FIXED) || !map_anon(NULL, pg, 0) ||
            !map_anon(pair, pg, MAP_FIXED))
            return -1;
        if (page_paddr(pair, pa0) != 0 || page_paddr(pair + pg, pa1) != 0)
            return -1;
        if (*pa1 != *pa0 + (off_t) pg) {
            *pairp = pair;
            return 0;
        }
    }
    return 1;
}

/* Check that the physical part (pa, len), which must lie within one page,
 * holds want, by mapping the frame it names. This is how a receiver of physical
 * parts reaches the bytes, so it is the check that the walk named the right
 * memory, not just plausible numbers.
 */
static int part_holds(off_t pa, size_t len, const char *want, size_t pg)
{
    off_t page = pa & ~(off_t) (pg - 1);
    size_t off = (size_t) (pa - page);
    char *map;
    int same;

    map = mmap(NULL, pg, PROT_READ, MAP_PHYS | MAP_SHARED, NOFD, page);
    if (map == MAP_FAILED) {
        fprintf(stderr, "ipc-roundtrip: straddle MAP_PHYS %#llx: %s\n",
                (unsigned long long) page, strerror(errno));
        return 0;
    }
    same = memcmp(map + off, want, len) == 0;
    (void) munmap(map, pg);
    return same;
}

/* Receive one message into a one-byte buffer, leaving its client reply
 * blocked. The receive is bounded so a sender that failed before sending
 * cannot hang the run. Returns the rcvid, or -1.
 */
static int receive_timed(int chid)
{
    const uint64_t timeout = STRADDLE_TIMEOUT_NS;
    struct _msg_info info;
    char rbuf[1];
    int rcvid;

    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL,
                           &timeout, NULL);
    rcvid = msg_receive(chid, rbuf, sizeof rbuf, &info);
    if (rcvid == -1)
        fprintf(stderr, "ipc-roundtrip: readiov msg_receive: %s\n",
                strerror(errno));
    return rcvid;
}

/* Receive one message, ask for it as physical parts from offset, and reply.
 * Returns msg_readiov's result with its errno in *err, or -2 if the receive or
 * the reply failed.
 */
static int receive_readiov(int chid,
                           iov_t *piov,
                           int parts,
                           int offset,
                           int *err)
{
    int rcvid, n;

    rcvid = receive_timed(chid);
    if (rcvid == -1)
        return -2;
    memset(piov, 0, (size_t) parts * sizeof *piov);
    n = msg_readiov(rcvid, piov, parts, offset, RTMUX_READIOV_SEND, NULL);
    *err = n == -1 ? errno : 0;
    if (msg_reply(rcvid, 0, NULL, 0) == -1) {
        fprintf(stderr, "ipc-roundtrip: straddle msg_reply: %s\n",
                strerror(errno));
        return -2;
    }
    return n;
}

static int check_readiov_straddle(int chid)
{
    const size_t pg = __PAGESIZE;
    struct straddle_ctx ctx = {.chid = chid, .rc = -1};
    iov_t piov[4];
    pthread_t tid;
    off_t pa0, pa1, p0, p1;
    size_t l0, l1;
    int n, err, short_ok, ok;
    char *pair;

    n = map_split_pair(pg, &pair, &pa0, &pa1);
    if (n == 1) {
        struct test_counts tc = {0};
        char why[96];

        snprintf(why, sizeof why,
                 "no non-adjacent frame pair in %d tries (last %#llx, %#llx)",
                 STRADDLE_TRIES, (unsigned long long) pa0,
                 (unsigned long long) pa1);
        tc_skip(&tc, "ipc-roundtrip: readiov-straddle", why);
        return 0;
    }
    if (n != 0)
        return -1;
    ctx.src = pair + pg - STRADDLE_HEAD;
    for (int i = 0; i < STRADDLE_LEN; i++)
        ctx.src[i] = (char) (0x40 + i);

    if (pthread_create(&tid, NULL, straddle_sender, &ctx) != EOK) {
        fprintf(stderr, "ipc-roundtrip: straddle pthread_create failed\n");
        return -1;
    }

    n = receive_readiov(chid, piov, 4, 0, &err);
    if (n == -2)
        return -1;
    short_ok = n == -1 && err == EINVAL;
    printf("ipc-roundtrip: readiov-short %s result=%d errno=%d\n",
           short_ok ? "PASS" : "FAIL", n, err);

    n = receive_readiov(chid, piov, 4, STRADDLE_PAD, &err);
    if (n == -2)
        return -1;
    (void) pthread_join(tid, NULL);
    p0 = (off_t) (uintptr_t) GETIOVBASE(&piov[0]);
    p1 = (off_t) (uintptr_t) GETIOVBASE(&piov[1]);
    l0 = GETIOVLEN(&piov[0]);
    l1 = GETIOVLEN(&piov[1]);
    ok = ctx.rc == 0 && n == 2 && p0 == pa0 + (off_t) (pg - STRADDLE_HEAD) &&
         l0 == STRADDLE_HEAD && p1 == pa1 &&
         l1 == STRADDLE_LEN - STRADDLE_HEAD &&
         part_holds(p0, l0, ctx.src, pg) &&
         part_holds(p1, l1, ctx.src + STRADDLE_HEAD, pg);
    printf(
        "ipc-roundtrip: readiov-straddle %s parts=%d pa0=%#llx pa1=%#llx "
        "part0=%#llx/%zu part1=%#llx/%zu\n",
        ok ? "PASS" : "FAIL", n, (unsigned long long) pa0,
        (unsigned long long) pa1, (unsigned long long) p0, l0,
        (unsigned long long) p1, l1);
    (void) munmap(pair, 2 * pg);
    return short_ok && ok ? 0 : -1;
}

/* msg_readiov over a large message. A sender thread sends one LARGE_LEN part,
 * and the receiver asks for it as physical parts, with room for one part per
 * page so that only the kernel's per-call cap ends a call early. The receiver
 * continues at the offset the returned parts reach, as any caller must. The
 * rdtime span of each call bounds that call's non-preemptible span; the p99
 * and the longest over LARGE_PASSES walks are reported. A walk that takes one
 * call means the cap is gone, so the check requires more than one.
 */
#define LARGE_LEN (64u << 20)
#define LARGE_PASSES 4

struct large_ctx {
    int chid;
    const iov_t *siov;
    int sparts;
    int rc;
};

static void *large_sender(void *arg)
{
    struct large_ctx *ctx = arg;
    int coid;

    ctx->rc = -1;
    coid = connect_attach((uint32_t) 0, 0, ctx->chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        fprintf(stderr, "ipc-roundtrip: large connect_attach: %s\n",
                strerror(errno));
        return NULL;
    }
    if (msg_sendv(coid, ctx->siov, ctx->sparts, NULL, 0) == -1)
        fprintf(stderr, "ipc-roundtrip: large msg_sendv: %s\n",
                strerror(errno));
    else
        ctx->rc = 0;
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

/* Check that the physical part (pa, len) maps src[0, len): mem_offset must put
 * src at pa and report at least len physically contiguous bytes from there.
 */
static int large_part_ok(const char *src, off_t pa, size_t len)
{
    size_t contig;
    off_t first;

    if (mem_offset(src, NOFD, len, &first, &contig) == -1)
        return 0;
    return first == pa && contig >= len;
}

/* Walk the received message once from offset 0. Returns 0 when the parts cover
 * exactly LARGE_LEN bytes and, if verify is set, every part names the frames
 * that map the bytes it covers. *calls counts the walk's calls, and each
 * call's duration is appended to ticks at *nticks. A walk takes at most parts
 * calls, since each call covers at least one page.
 */
static int large_walk(int rcvid,
                      iov_t *piov,
                      int parts,
                      const char *src,
                      int verify,
                      int *calls,
                      uint64_t *ticks,
                      size_t *nticks)
{
    size_t covered = 0;

    *calls = 0;
    while (covered < LARGE_LEN) {
        uint64_t t0, dt;
        int n;

        if (*calls == parts) {
            fprintf(stderr, "ipc-roundtrip: large walk overran at %zu\n",
                    covered);
            return -1;
        }
        t0 = read_time();
        n = msg_readiov(rcvid, piov, parts, (int) covered, RTMUX_READIOV_SEND,
                        NULL);
        dt = read_time() - t0;
        ++*calls;
        ticks[(*nticks)++] = dt;
        if (n <= 0) {
            fprintf(stderr, "ipc-roundtrip: large msg_readiov at %zu: %d %s\n",
                    covered, n, n == -1 ? strerror(errno) : "");
            return -1;
        }
        if (n > parts) {
            fprintf(stderr, "ipc-roundtrip: large walk overran at %zu\n",
                    covered);
            return -1;
        }
        for (int i = 0; i < n; i++) {
            off_t pa = (off_t) (uintptr_t) GETIOVBASE(&piov[i]);
            size_t len = GETIOVLEN(&piov[i]);

            if (len == 0 || len > LARGE_LEN - covered ||
                (verify && !large_part_ok(src + covered, pa, len))) {
                fprintf(stderr,
                        "ipc-roundtrip: large bad part %d at %zu: %#llx/%zu\n",
                        i, covered, (unsigned long long) pa, len);
                return -1;
            }
            covered += len;
        }
    }
    return 0;
}

/* The offset skip. The same 64 MiB is sent as SKIP_PARTS entries of SKIP_ENTRY
 * bytes. A call skips at most SKIP_BUDGET entries, so the check
 *  - asks without a cursor for the last entry and expects E2BIG, and for an
 *    entry inside the budget and expects it to map;
 *  - asks with a cursor for the last entry and expects EINTR until the cursor
 *    reaches it, in about SKIP_PARTS / SKIP_BUDGET calls, then one part;
 *  - walks the whole message with one cursor, which takes a number of calls
 *    linear in SKIP_PARTS, not one that grows with each call's offset.
 * It reports the p99 and max time of the calls in the walks.
 */
#define SKIP_ENTRY 1024u
#define SKIP_PARTS (LARGE_LEN / SKIP_ENTRY)
#define SKIP_BUDGET 128u
#define SKIP_WALK_CAP 16
/* Most calls a cold jump to the last entry may take. */
#define SKIP_COLD_CALLS_MAX ((int) (SKIP_PARTS / SKIP_BUDGET + 2))
#define SKIP_CALLS_MAX (SKIP_PARTS / 16u)
#define SKIP_TICKS_MAX (SKIP_CALLS_MAX + SKIP_PARTS / SKIP_BUDGET + 8)

/* One msg_readiov at offset, timed into ticks[*nticks] when ticks is set. */
static int skip_call(int rcvid,
                     iov_t *piov,
                     int parts,
                     size_t offset,
                     struct _iov_cursor *cur,
                     uint64_t *ticks,
                     size_t *nticks)
{
    uint64_t t0 = read_time();
    int n = msg_readiov(rcvid, piov, parts, (int) offset, RTMUX_READIOV_SEND,
                        cur);

    if (ticks)
        ticks[(*nticks)++] = read_time() - t0;
    return n;
}

static int skip_checks(int rcvid,
                       char *src,
                       uint64_t *ticks,
                       size_t *nticks,
                       int *calls)
{
    struct _iov_cursor cur = {0};
    iov_t piov[SKIP_WALK_CAP];
    size_t covered = 0;
    int n, ncalls = 0;

    n = skip_call(rcvid, piov, 2, LARGE_LEN - SKIP_ENTRY, NULL, NULL, NULL);
    if (n != -1 || errno != E2BIG) {
        fprintf(stderr, "ipc-roundtrip: skip no-cursor: %d %s, want E2BIG\n",
                n, n == -1 ? strerror(errno) : "");
        return -1;
    }
    n = skip_call(rcvid, piov, 2, (SKIP_BUDGET - 1) * SKIP_ENTRY, NULL, NULL,
                  NULL);
    if (n < 1 ||
        !large_part_ok(src + (SKIP_BUDGET - 1) * SKIP_ENTRY,
                       (off_t) (uintptr_t) GETIOVBASE(&piov[0]), SKIP_ENTRY)) {
        fprintf(stderr, "ipc-roundtrip: skip within budget: %d\n", n);
        return -1;
    }
    /* A cursor is untrusted: each of these must fail with EINVAL. */
    for (unsigned k = 0; k < 5; k++) {
        static const struct _iov_cursor bad[5] = {
            {.entry = SKIP_PARTS + 1},                /* past the iov */
            {.entry = 3, .start = 1u << 20},          /* start past offset */
            {.entry = 0, .start = 1},                 /* start with no entry */
            {.entry = 1, .start = 1024, .spare = 1},  /* reserved not zero */
            {.entry = 1, .start = UINT64_MAX},        /* overflowing start */
        };
        struct _iov_cursor c = bad[k];

        n = skip_call(rcvid, piov, 2, 2 * SKIP_ENTRY, &c, NULL, NULL);
        if (n != -1 || errno != EINVAL) {
            fprintf(stderr, "ipc-roundtrip: skip bad cursor %u: %d %s\n", k, n,
                    n == -1 ? strerror(errno) : "");
            return -1;
        }
    }
    /* Cold jump to the last entry: EINTR until the cursor gets there. */
    do {
        n = skip_call(rcvid, piov, 2, LARGE_LEN - SKIP_ENTRY, &cur, ticks,
                      nticks);
        ncalls++;
    } while (n == -1 && errno == EINTR && ncalls <= SKIP_COLD_CALLS_MAX);
    if (n != 1 || GETIOVLEN(&piov[0]) != SKIP_ENTRY ||
        !large_part_ok(src + LARGE_LEN - SKIP_ENTRY,
                       (off_t) (uintptr_t) GETIOVBASE(&piov[0]), SKIP_ENTRY) ||
        ncalls > SKIP_COLD_CALLS_MAX) {
        fprintf(stderr, "ipc-roundtrip: skip cold: n=%d calls=%d\n", n,
                ncalls);
        return -1;
    }
    *calls = ncalls;
    /* Whole message, one cursor. */
    memset(&cur, 0, sizeof cur);
    ncalls = 0;
    while (covered < LARGE_LEN) {
        n = skip_call(rcvid, piov, SKIP_WALK_CAP, covered, &cur, ticks,
                      nticks);
        if (++ncalls > (int) SKIP_CALLS_MAX ||
            (n == -1 && errno != EINTR) || n == 0) {
            fprintf(stderr,
                    "ipc-roundtrip: skip walk at %zu: n=%d calls=%d %s\n",
                    covered, n, ncalls, n == -1 ? strerror(errno) : "");
            return -1;
        }
        for (int i = 0; i < n; i++)
            covered += GETIOVLEN(&piov[i]);
    }
    *calls += ncalls;
    return covered == LARGE_LEN ? 0 : -1;
}

static int check_readiov_skip(int chid, char *src)
{
    struct large_ctx ctx = {.chid = chid, .rc = -1};
    uint64_t cps = cycles_per_sec(), p99_ns = 0, max_ns = 0, *ticks;
    size_t nticks = 0;
    iov_t *siov;
    pthread_t tid;
    int rcvid, ok, calls = 0;

    siov = malloc(SKIP_PARTS * sizeof *siov);
    ticks = malloc(SKIP_TICKS_MAX * sizeof *ticks);
    if (!siov || !ticks) {
        fprintf(stderr, "ipc-roundtrip: skip: no memory\n");
        free(siov);
        free(ticks);
        return -1;
    }
    for (size_t i = 0; i < SKIP_PARTS; i++)
        SETIOV(&siov[i], src + i * SKIP_ENTRY, SKIP_ENTRY);
    ctx.siov = siov;
    ctx.sparts = (int) SKIP_PARTS;
    if (pthread_create(&tid, NULL, large_sender, &ctx) != EOK) {
        fprintf(stderr, "ipc-roundtrip: skip pthread_create failed\n");
        free(siov);
        free(ticks);
        return -1;
    }
    /* No receive: the sender may still be reading siov, so leave it. */
    if ((rcvid = receive_timed(chid)) == -1)
        return -1;
    ok = skip_checks(rcvid, src, ticks, &nticks, &calls) == 0;
    if (msg_reply(rcvid, 0, NULL, 0) == -1) {
        fprintf(stderr, "ipc-roundtrip: skip msg_reply: %s\n",
                strerror(errno));
        ok = 0;
    }
    (void) pthread_join(tid, NULL);
    ok = ok && ctx.rc == 0;
    if (nticks && cps) {
        qsort(ticks, nticks, sizeof *ticks, cmp_u64);
        p99_ns = ticks_to_ns(ticks[p99_index(nticks)], cps);
        max_ns = ticks_to_ns(ticks[nticks - 1], cps);
    }
    printf("ipc-roundtrip: readiov-skip %s entries=%u calls=%d samples=%zu "
           "p99-call-ns=%llu max-call-ns=%llu\n",
           ok ? "PASS" : "FAIL", SKIP_PARTS, calls, nticks,
           (unsigned long long) p99_ns, (unsigned long long) max_ns);
    free(siov);
    free(ticks);
    return ok ? 0 : -1;
}

/* A zero-length entry after the first part is a walk step, not a skip: with the
 * skip budget almost spent it must not turn a finished part into EINTR. The
 * message is SKIP_BUDGET entries, an empty one, then a last entry; the call
 * skips SKIP_BUDGET - 1 entries and has to return the part it found.
 */
static int check_readiov_empty(int chid, char *src)
{
    /* static: on a receive timeout the sender is left running and still reads
     * both, as in the other checks.
     */
    static struct large_ctx ctx;
    static iov_t siov[SKIP_BUDGET + 2];
    struct _iov_cursor cur = {0};
    iov_t piov[4];
    pthread_t tid;
    int rcvid, n, ok;

    ctx.chid = chid;
    ctx.rc = -1;
    for (unsigned i = 0; i < SKIP_BUDGET; i++)
        SETIOV(&siov[i], src + i * SKIP_ENTRY, SKIP_ENTRY);
    SETIOV(&siov[SKIP_BUDGET], src, 0);
    SETIOV(&siov[SKIP_BUDGET + 1], src + SKIP_BUDGET * SKIP_ENTRY, SKIP_ENTRY);
    ctx.siov = siov;
    ctx.sparts = SKIP_BUDGET + 2;
    if (pthread_create(&tid, NULL, large_sender, &ctx) != EOK) {
        fprintf(stderr, "ipc-roundtrip: empty pthread_create failed\n");
        return -1;
    }
    /* No receive: the sender may still be reading siov, so leave it. */
    if ((rcvid = receive_timed(chid)) == -1)
        return -1;
    n = skip_call(rcvid, piov, 4, (SKIP_BUDGET - 1) * SKIP_ENTRY, &cur, NULL,
                  NULL);
    ok = n >= 1 && cur.entry >= SKIP_BUDGET - 1;
    if (!ok)
        fprintf(stderr, "ipc-roundtrip: empty: n=%d %s entry=%u\n", n,
                n == -1 ? strerror(errno) : "", cur.entry);
    if (msg_reply(rcvid, 0, NULL, 0) == -1)
        ok = 0;
    (void) pthread_join(tid, NULL);
    ok = ok && ctx.rc == 0;
    printf("ipc-roundtrip: readiov-empty %s parts=%d entry=%u\n",
           ok ? "PASS" : "FAIL", n, cur.entry);
    return ok ? 0 : -1;
}

static int check_readiov_large(int chid)
{
    const size_t pg = __PAGESIZE;
    const int parts = (int) (LARGE_LEN / pg);
    struct large_ctx ctx = {.chid = chid, .rc = -1};
    char *src;
    uint64_t cps = cycles_per_sec(), p99_ns = 0, max_ns = 0, *ticks;
    size_t nticks = 0;
    int calls = 0, ok = 1;
    pthread_t tid;
    iov_t *piov, whole;
    int rcvid;

    src = mmap(NULL, LARGE_LEN, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, NOFD, 0);
    piov = malloc((size_t) parts * sizeof *piov);
    ticks = malloc((size_t) LARGE_PASSES * (size_t) parts * sizeof *ticks);
    if (src == MAP_FAILED || !piov || !ticks) {
        fprintf(stderr, "ipc-roundtrip: large: no memory\n");
        goto fail;
    }
    for (size_t off = 0; off < LARGE_LEN; off += pg)
        src[off] = (char) (off / pg);
    SETIOV(&whole, src, LARGE_LEN);
    ctx.siov = &whole;
    ctx.sparts = 1;
    if (pthread_create(&tid, NULL, large_sender, &ctx) != EOK) {
        fprintf(stderr, "ipc-roundtrip: large pthread_create failed\n");
        goto fail;
    }
    /* No receive: the sender may still be reading src, so leave it mapped. */
    if ((rcvid = receive_timed(chid)) == -1) {
        free(ticks);
        free(piov);
        return -1;
    }
    for (int pass = 0; pass < LARGE_PASSES && ok; pass++)
        ok = large_walk(rcvid, piov, parts, src, pass == 0, &calls, ticks,
                        &nticks) == 0;
    if (msg_reply(rcvid, 0, NULL, 0) == -1) {
        fprintf(stderr, "ipc-roundtrip: large msg_reply: %s\n",
                strerror(errno));
        ok = 0;
    }
    (void) pthread_join(tid, NULL);
    ok = ok && ctx.rc == 0 && calls > 1;
    if (nticks && cps) {
        qsort(ticks, nticks, sizeof *ticks, cmp_u64);
        p99_ns = ticks_to_ns(ticks[p99_index(nticks)], cps);
        max_ns = ticks_to_ns(ticks[nticks - 1], cps);
    }
    printf("ipc-roundtrip: readiov-large %s bytes=%u calls=%d samples=%zu "
           "p99-call-ns=%llu max-call-ns=%llu\n",
           ok ? "PASS" : "FAIL", LARGE_LEN, calls, nticks,
           (unsigned long long) p99_ns, (unsigned long long) max_ns);
    free(ticks);
    free(piov);
    if (ok)
        ok = check_readiov_skip(chid, src) == 0 &&
             check_readiov_empty(chid, src) == 0;
    (void) munmap(src, LARGE_LEN);
    return ok ? 0 : -1;

fail:
    free(ticks);
    free(piov);
    if (src != MAP_FAILED)
        (void) munmap(src, LARGE_LEN);
    return -1;
}

/* msg_readv and msg_writev over an iov array of IOV_PARTS one-byte entries. A
 * sender thread sends IOV_PARTS bytes and waits for a reply buffer of as many.
 * The receiver reads the message through the array, checks the bytes, writes
 * the complement back through the array, and the sender checks that. The
 * kernel checks such an array in steps of IOV_STEP entries per kernel entry
 * and resumes after each, so the result must be the same as for one entry. An
 * entry past the first step that lies outside the process, and an array whose
 * tail page is unmapped, must fail with EFAULT: the first rather than being
 * followed, the second rather than faulting the kernel.
 *
 * The longest kernel entry of the check is one full step, so a call over
 * exactly IOV_STEP entries whose last one is bad runs one step and fails; its
 * p99 over IOV_STEP_CALLS calls must stay under IOV_STEP_NS_MAX. The same
 * failure at the end of the whole array (every step) is timed for the record.
 * The sender's own vectors have one-byte entries too, so a copy of
 * IOV_COPY_PARTS bytes steps over IOV_STEP entries, one step of copy work. The
 * copy saves a cursor and restarts the call after each step, so a call that
 * short makes one kernel entry and its p99 is the longest entry of a long
 * one; it must stay under IOV_STEP_NS_MAX as well. The whole calls are timed
 * for the record.
 */
#define IOV_PARTS 65536
#define IOV_STEP 512
#define IOV_COPY_PARTS 64
#define IOV_STEP_CALLS 64
#define IOV_PASSES 8
#define IOV_STEP_NS_MAX 250000ULL
#define IOV_BAD_AT 700
#define IOV_TAIL_PARTS 64
#define IOV_KERNEL_VA ((void *) 0xffffffc000000000UL)

struct iov_ctx {
    int chid;
    unsigned char *out, *in;
    iov_t *sarr, *rarr; /* the sender's vectors, one byte per entry */
    int rc;
};

static void *iov_sender(void *arg)
{
    struct iov_ctx *ctx = arg;
    iov_t siov, riov;
    int coid, r;

    ctx->rc = -1;
    coid = connect_attach((uint32_t) 0, 0, ctx->chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1) {
        fprintf(stderr, "ipc-roundtrip: iov connect_attach: %s\n",
                strerror(errno));
        return NULL;
    }
    SETIOV(&siov, ctx->out, IOV_PARTS);
    SETIOV(&riov, ctx->in, IOV_PARTS);
    if (ctx->sarr && ctx->rarr)
        r = msg_sendv(coid, ctx->sarr, IOV_PARTS, ctx->rarr, IOV_PARTS);
    else
        r = msg_sendv(coid, &siov, 1, &riov, 1);
    if (r == -1)
        fprintf(stderr, "ipc-roundtrip: iov msg_sendv: %s\n", strerror(errno));
    else
        ctx->rc = 0;
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

static int iov_call(int rcvid, int write, iov_t *iov, int parts)
{
    return write ? msg_writev(rcvid, iov, parts, 0)
                 : msg_readv(rcvid, iov, parts, 0);
}

/* One call that must fail with EFAULT, whichever of readv and writev it is. */
static int iov_expect_efault(int rcvid,
                             int write,
                             iov_t *iov,
                             int parts,
                             const char *what)
{
    int n = iov_call(rcvid, write, iov, parts);

    if (n == -1 && errno == EFAULT)
        return 0;
    fprintf(stderr, "ipc-roundtrip: iov %s %s: %d %s, want EFAULT\n", what,
            write ? "writev" : "readv", n, n == -1 ? strerror(errno) : "");
    return -1;
}

/* An array whose first IOV_TAIL_PARTS / 2 entries are readable and whose rest
 * lie on an unmapped page, a one-entry array on an unmapped page, an array in
 * kernel space or wrapping past the top of it, and a long
 * array with one bad entry past the first step.
 */
static int iov_fault_checks(int rcvid, iov_t *iov, unsigned char *buf)
{
    const size_t pg = __PAGESIZE;
    const size_t head = (IOV_TAIL_PARTS / 2) * sizeof(iov_t);
    char *map = mmap(NULL, 2 * pg, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON, NOFD, 0);
    iov_t *tail, saved;
    int rc = 0;

    if (map == MAP_FAILED) {
        fprintf(stderr, "ipc-roundtrip: iov mmap: %s\n", strerror(errno));
        return -1;
    }
    (void) munmap(map + pg, pg);
    tail = (iov_t *) (void *) (map + pg - head);
    for (int i = 0; i < IOV_TAIL_PARTS / 2; i++)
        SETIOV(&tail[i], buf + i, 1);
    for (int w = 0; w < 2; w++)
        rc |= iov_expect_efault(rcvid, w, tail, IOV_TAIL_PARTS,
                                "unmapped tail");
    for (int w = 0; w < 2; w++)
        rc |= iov_expect_efault(rcvid, w, (iov_t *) (void *) (map + pg), 1,
                                "unmapped single entry");
    (void) munmap(map, pg);
    for (int w = 0; w < 2; w++) {
        rc |= iov_expect_efault(rcvid, w, IOV_KERNEL_VA, IOV_TAIL_PARTS,
                                "kernel array");
        rc |= iov_expect_efault(rcvid, w, (iov_t *) (void *) ~(uintptr_t) 0xff,
                                IOV_TAIL_PARTS, "wrapping array");
    }

    saved = iov[IOV_BAD_AT];
    SETIOV(&iov[IOV_BAD_AT], IOV_KERNEL_VA, 8);
    for (int w = 0; w < 2; w++)
        rc |= iov_expect_efault(rcvid, w, iov, IOV_PARTS, "kernel entry");
    iov[IOV_BAD_AT] = saved;
    return rc;
}

/* Time calls over the first parts entries of iov with the last one outside the
 * process, so that the check runs to its end and fails; the p99 goes to *p99,
 * the slowest to *max.
 */
static int iov_check_time(int rcvid,
                          int write,
                          iov_t *iov,
                          int parts,
                          uint64_t cps,
                          uint64_t *p99,
                          uint64_t *max)
{
    static uint64_t ns[IOV_STEP_CALLS];
    iov_t saved = iov[parts - 1];
    int rc = 0;

    SETIOV(&iov[parts - 1], IOV_KERNEL_VA, 8);
    for (int i = 0; i < IOV_STEP_CALLS && !rc; i++) {
        uint64_t t0 = read_time();

        rc = iov_expect_efault(rcvid, write, iov, parts, "last entry");
        ns[i] = ticks_to_ns(read_time() - t0, cps);
    }
    iov[parts - 1] = saved;
    qsort(ns, IOV_STEP_CALLS, sizeof ns[0], cmp_u64);
    *p99 = ns[p99_index(IOV_STEP_CALLS)];
    *max = ns[IOV_STEP_CALLS - 1];
    return rc;
}

/* Time IOV_PASSES whole calls over iov; the slowest goes to *max_ns. */
static int iov_timed(int rcvid,
                     int write,
                     iov_t *iov,
                     uint64_t cps,
                     uint64_t *max_ns)
{
    *max_ns = 0;
    for (int pass = 0; pass < IOV_PASSES; pass++) {
        uint64_t t0 = read_time(), ns;
        int n = iov_call(rcvid, write, iov, IOV_PARTS);

        ns = ticks_to_ns(read_time() - t0, cps);
        if (n != IOV_PARTS) {
            fprintf(stderr, "ipc-roundtrip: iov %s: %d %s\n",
                    write ? "writev" : "readv", n,
                    n == -1 ? strerror(errno) : "");
            return -1;
        }
        if (ns > *max_ns)
            *max_ns = ns;
    }
    return 0;
}

/* The sender's vectors are shared with its siblings, here the receiver. An
 * entry rewritten to point into the kernel after the sender blocked is not
 * covered by the check of the caller's own vector, so the copy has to check
 * it: the call must fail with EFAULT, not move kernel memory. Both
 * directions: readv reads the sender's send vector, writev writes its reply
 * vector.
 */
static int iov_peer_checks(int rcvid,
                           iov_t *rdiov,
                           iov_t *wriov,
                           iov_t *sarr,
                           iov_t *rarr)
{
    int rc = 0;

    for (int w = 0; w < 2; w++) {
        iov_t *peer = w ? rarr : sarr;
        iov_t saved = peer[IOV_BAD_AT];

        SETIOV(&peer[IOV_BAD_AT], IOV_KERNEL_VA, 8);
        rc |= iov_expect_efault(rcvid, w, w ? wriov : rdiov, IOV_PARTS,
                                "peer entry rewritten");
        peer[IOV_BAD_AT] = saved;
    }
    return rc;
}

/* A sibling that flips an entry of the sender's send vector between a good and
 * a kernel address while the receiver reads through it. Each call has to be a
 * whole good transfer or EFAULT, and the kernel has to survive: the entry is
 * read once per use and checked as it is used. The flipper is a thread above
 * the receiver that wakes on a timer, so it also shows that the long call can
 * be preempted between its steps.
 */
#define IOV_RACE_CALLS 12
#define IOV_RACE_PRIORITY (SERVER_PRIORITY + 2)

struct iov_race {
    iov_t *entry, good;
    volatile int stop;
};

static void *iov_flipper(void *arg)
{
    struct iov_race *r = arg;
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 300000};

    (void) raise_to_rt(IOV_RACE_PRIORITY);
    while (!r->stop) {
        SETIOV(r->entry, IOV_KERNEL_VA, 8);
        (void) nanosleep(&ts, NULL);
        *r->entry = r->good;
        (void) nanosleep(&ts, NULL);
    }
    *r->entry = r->good;
    return NULL;
}

static int iov_race_check(int rcvid,
                          iov_t *rdiov,
                          unsigned char *rd,
                          const unsigned char *out,
                          iov_t *sarr)
{
    struct iov_race race = {.entry = &sarr[IOV_PARTS - 2],
                            .good = sarr[IOV_PARTS - 2]};
    int efault = 0, good = 0, rc = 0;
    pthread_t tid;

    if (pthread_create(&tid, NULL, iov_flipper, &race) != EOK)
        return -1;
    for (int i = 0; i < IOV_RACE_CALLS && !rc; i++) {
        int n;

        memset(rd, 0, IOV_PARTS);
        n = msg_readv(rcvid, rdiov, IOV_PARTS, 0);
        if (n == -1 && errno == EFAULT) {
            efault++;
        } else if (n == IOV_PARTS && memcmp(rd, out, IOV_PARTS) == 0) {
            good++;
        } else {
            fprintf(stderr, "ipc-roundtrip: iov race readv: %d %s\n", n,
                    n == -1 ? strerror(errno) : "");
            rc = -1;
        }
    }
    race.stop = 1;
    (void) pthread_join(tid, NULL);

    /* The flipper wakes on a timer and is above the receiver: it only gets to
     * poison the entry in the middle of a call if the call lets it run. Calls
     * that all see the entry good mean it never ran, that is the call does not
     * stop for a higher-priority thread between its steps.
     */
    if (!rc && !efault) {
        fprintf(stderr, "ipc-roundtrip: iov race: the flipper never ran\n");
        rc = -1;
    }
    printf("ipc-roundtrip: iov-race %s calls=%d efault=%d good=%d\n",
           rc ? "FAIL" : "PASS", IOV_RACE_CALLS, efault, good);
    return rc;
}

/* Time IOV_STEP_CALLS whole calls over parts entries, each of which must move
 * want bytes; the p99 goes to *p99 and the slowest to *max. With
 * IOV_COPY_PARTS entries that is one step of copy work, counting the entries
 * of both vectors. A call this short makes one kernel entry, so its span
 * is the longest entry the copy of a longer one makes.
 */
static int iov_copy_time(int rcvid,
                         int write,
                         iov_t *iov,
                         int parts,
                         int want,
                         uint64_t cps,
                         uint64_t *p99,
                         uint64_t *max)
{
    static uint64_t ns[IOV_STEP_CALLS];

    for (int i = 0; i < IOV_STEP_CALLS; i++) {
        uint64_t t0 = read_time();
        int n = iov_call(rcvid, write, iov, parts);

        ns[i] = ticks_to_ns(read_time() - t0, cps);
        if (n != want) {
            fprintf(stderr, "ipc-roundtrip: iov copy %s: %d %s\n",
                    write ? "writev" : "readv", n,
                    n == -1 ? strerror(errno) : "");
            return -1;
        }
    }
    qsort(ns, IOV_STEP_CALLS, sizeof ns[0], cmp_u64);
    *p99 = ns[p99_index(IOV_STEP_CALLS)];
    *max = ns[IOV_STEP_CALLS - 1];
    return 0;
}

static int check_iov_steps(int chid)
{
    static unsigned char out[IOV_PARTS], in[IOV_PARTS], rd[IOV_PARTS],
        wr[IOV_PARTS];
    static iov_t rdiov[IOV_PARTS], wriov[IOV_PARTS];
    static iov_t sarr[IOV_PARTS], rarr[IOV_PARTS];
    static struct iov_ctx ctx;
    uint64_t cps = cycles_per_sec(), rd_ns = 0, wr_ns = 0;
    uint64_t step_p99 = 0, step_max = 0, all_p99 = 0, all_max = 0;
    uint64_t copy_p99 = 0, copy_max = 0;
    pthread_t tid;
    int rcvid, ok;

    ctx = (struct iov_ctx){.chid = chid,
                           .out = out,
                           .in = in,
                           .sarr = sarr,
                           .rarr = rarr,
                           .rc = -1};
    for (int i = 0; i < IOV_PARTS; i++) {
        out[i] = (unsigned char) (i * 7 + 1);
        wr[i] = (unsigned char) ~out[i];
        SETIOV(&rdiov[i], rd + i, 1);
        SETIOV(&wriov[i], wr + i, 1);
        SETIOV(&sarr[i], out + i, 1);
        SETIOV(&rarr[i], in + i, 1);
    }
    if (pthread_create(&tid, NULL, iov_sender, &ctx) != EOK) {
        fprintf(stderr, "ipc-roundtrip: iov pthread_create failed\n");
        return -1;
    }
    /* static buffers: on a receive timeout the sender is left running. */
    if ((rcvid = receive_timed(chid)) == -1)
        return -1;
    ok = iov_fault_checks(rcvid, rdiov, rd) == 0;
    for (int w = 0; w < 2; w++) {
        iov_t *iov = w ? wriov : rdiov;
        uint64_t p99 = 0, max = 0;

        ok = ok && iov_check_time(rcvid, w, iov, IOV_STEP, cps, &p99, &max) == 0;
        if (p99 > step_p99)
            step_p99 = p99;
        if (max > step_max)
            step_max = max;
        ok = ok && iov_check_time(rcvid, w, iov, IOV_PARTS, cps, &p99, &max) == 0;
        if (p99 > all_p99)
            all_p99 = p99;
        if (max > all_max)
            all_max = max;
    }
    ok = ok && step_p99 < IOV_STEP_NS_MAX &&
         iov_peer_checks(rcvid, rdiov, wriov, sarr, rarr) == 0 &&
         iov_race_check(rcvid, rdiov, rd, out, sarr) == 0;
    for (int w = 0; w < 2; w++) {
        uint64_t p99 = 0, max = 0;

        ok = ok && iov_copy_time(rcvid, w, w ? wriov : rdiov, IOV_COPY_PARTS,
                                 IOV_COPY_PARTS, cps, &p99, &max) == 0;
        if (p99 > copy_p99)
            copy_p99 = p99;
        if (max > copy_max)
            copy_max = max;
    }
    ok = ok && copy_p99 < IOV_STEP_NS_MAX &&
         iov_timed(rcvid, 0, rdiov, cps, &rd_ns) == 0 &&
         memcmp(rd, out, IOV_PARTS) == 0 &&
         iov_timed(rcvid, 1, wriov, cps, &wr_ns) == 0;
    if (msg_reply(rcvid, 0, NULL, 0) == -1) {
        fprintf(stderr, "ipc-roundtrip: iov msg_reply: %s\n", strerror(errno));
        ok = 0;
    }
    (void) pthread_join(tid, NULL);
    ok = ok && ctx.rc == 0 && memcmp(in, wr, IOV_PARTS) == 0;
    printf("ipc-roundtrip: iov-steps %s parts=%d step-p99-ns=%llu "
           "step-max-ns=%llu check-p99-ns=%llu check-max-ns=%llu "
           "copy-p99-ns=%llu copy-max-ns=%llu readv-ns=%llu writev-ns=%llu\n",
           ok ? "PASS" : "FAIL", IOV_PARTS, (unsigned long long) step_p99,
           (unsigned long long) step_max, (unsigned long long) all_p99,
           (unsigned long long) all_max, (unsigned long long) copy_p99,
           (unsigned long long) copy_max, (unsigned long long) rd_ns,
           (unsigned long long) wr_ns);
    return ok ? 0 : -1;
}

/* The byte side of a step: a sender with one IOV_PARTS-byte entry in each
 * direction, and one-entry calls that move IOV_BYTE_STEP of it, which is the
 * XFER_STEP_BYTES of copy work the most a kernel entry copies. Their p99 must
 * stay under IOV_STEP_NS_MAX, and the bytes must arrive.
 */
#define IOV_BYTE_STEP (8 * 1024)

static int check_iov_bytes(int chid)
{
    static unsigned char out[IOV_PARTS], in[IOV_PARTS], rd[IOV_PARTS],
        wr[IOV_PARTS];
    static struct iov_ctx ctx;
    uint64_t cps = cycles_per_sec(), rp99 = 0, rmax = 0, wp99 = 0, wmax = 0;
    iov_t rdiov, wriov;
    pthread_t tid;
    int rcvid, ok;

    ctx = (struct iov_ctx){.chid = chid, .out = out, .in = in, .rc = -1};
    for (int i = 0; i < IOV_PARTS; i++) {
        out[i] = (unsigned char) (i * 7 + 1);
        wr[i] = (unsigned char) ~out[i];
    }
    SETIOV(&rdiov, rd, IOV_BYTE_STEP);
    SETIOV(&wriov, wr, IOV_BYTE_STEP);
    if (pthread_create(&tid, NULL, iov_sender, &ctx) != EOK) {
        fprintf(stderr, "ipc-roundtrip: iov pthread_create failed\n");
        return -1;
    }
    if ((rcvid = receive_timed(chid)) == -1)
        return -1;
    ok = iov_copy_time(rcvid, 0, &rdiov, 1, IOV_BYTE_STEP, cps, &rp99, &rmax) ==
             0 &&
         memcmp(rd, out, IOV_BYTE_STEP) == 0 &&
         iov_copy_time(rcvid, 1, &wriov, 1, IOV_BYTE_STEP, cps, &wp99, &wmax) ==
             0;
    if (msg_reply(rcvid, 0, NULL, 0) == -1) {
        fprintf(stderr, "ipc-roundtrip: iov msg_reply: %s\n", strerror(errno));
        ok = 0;
    }
    (void) pthread_join(tid, NULL);
    ok = ok && ctx.rc == 0 && memcmp(in, wr, IOV_BYTE_STEP) == 0 &&
         rp99 < IOV_STEP_NS_MAX && wp99 < IOV_STEP_NS_MAX;
    printf("ipc-roundtrip: iov-bytes %s bytes=%d readv-p99-ns=%llu "
           "readv-max-ns=%llu writev-p99-ns=%llu writev-max-ns=%llu\n",
           ok ? "PASS" : "FAIL", IOV_BYTE_STEP, (unsigned long long) rp99,
           (unsigned long long) rmax, (unsigned long long) wp99,
           (unsigned long long) wmax);
    return ok ? 0 : -1;
}

/* The other copying paths: a short message read into a vector of many entries
 * (msg_readv, whose destination of empty entries was walked to its end), and
 * the copy of send, receive and reply, which are stepped like msg_readv and
 * msg_writev. Each of them over IOV_COPY_PARTS entries on each side is one step
 * of work, so its p99 must stay under IOV_STEP_NS_MAX; the long calls must move
 * the bytes whatever the number of restarts. A msg_sendv whose arrays are not
 * inside the caller's space must fail with EFAULT, as msg_receivev's do.
 */
#define PATH_SHORT 32
#define PATH_ENTRY \
    16 /* so that PATH_PARTS entries make a long message (over 256 bytes) */
#define PATH_PARTS 32 /* each side: one step of work, timed */
#define PATH_MID 2048 /* each side: many steps, which must move the bytes */
#define PATH_BIG 4097 /* the same past the 4096 entries once copied whole */
#define PATH_BIG_ROUNDS 4
#define PATH_NAP_NS 2000000L
#define PATH_TOO_MANY 600000 /* more entries than a transfer takes */

static void path_nap(long ns)
{
    struct timespec ts = {.tv_sec = 0, .tv_nsec = ns};

    (void) nanosleep(&ts, NULL);
}

struct path_ctx {
    int chid, parts, entry, rounds;
    iov_t sarr[PATH_BIG], rarr[PATH_BIG];
    unsigned char out[PATH_BIG * PATH_ENTRY], in[PATH_BIG * PATH_ENTRY];
    volatile uint64_t t_send; /* when the sender entered msg_sendv */
    int rc;
};

static void *path_sender(void *arg)
{
    struct path_ctx *c = arg;
    int coid = connect_attach((uint32_t) 0, 0, c->chid, RTMUX_SIDE_CHANNEL, 0);

    c->rc = -1;
    if (coid == -1)
        return NULL;
    for (int i = 0; i < c->rounds; i++) {
        int r;

        path_nap(PATH_NAP_NS); /* the receiver is, or is not yet, receiving */
        (void) __timer_timeout(CLOCK_MONOTONIC,
                               RTMUX_TIMEOUT_SEND | RTMUX_TIMEOUT_REPLY, NULL,
                               &(const uint64_t){STRADDLE_TIMEOUT_NS}, NULL);
        c->t_send = read_time();
        r = msg_sendv(coid, c->sarr, c->parts, c->rarr, c->parts);
        if (r == -1 ||
            memcmp(c->in, c->out, (size_t) (c->parts * c->entry)) != 0) {
            fprintf(stderr,
                    "ipc-roundtrip: path send round %d parts %d: %d %s\n", i,
                    c->parts, r, r == -1 ? strerror(errno) : "reply differs");
            goto out;
        }
        memset(c->in, 0, sizeof c->in);
    }
    c->rc = 0;
out:
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

static int path_p99(uint64_t *ns, uint64_t *max)
{
    qsort(ns, IOV_STEP_CALLS, sizeof ns[0], cmp_u64);
    *max = ns[IOV_STEP_CALLS - 1];
    return ns[p99_index(IOV_STEP_CALLS)] >= IOV_STEP_NS_MAX;
}

static uint64_t path_base_ns; /* p99 delivery of one 512 byte entry */

/* A send, receive and reply of parts entries each way, with the receiver
 * waiting (receiver_first), or with the sender queued first, so that the
 * receive copies from the queue. The send's own cost is the time from the
 * sender's entry into msg_sendv to the receiver's return from its receive (the
 * copy, then the wake); in the second case the receive and the reply are timed
 * in the server. Over PATH_PARTS entries each is one step, and its p99 must
 * stay under IOV_STEP_NS_MAX; over PATH_BIG entries it is many, and the bytes
 * must arrive.
 */
static int path_round(int chid, int receiver_first, int parts, int entry)
{
    static struct path_ctx c;
    static unsigned char sin[PATH_BIG * PATH_ENTRY];
    static iov_t siov[PATH_BIG], riov[PATH_BIG];
    static uint64_t snd[IOV_STEP_CALLS], rcv[IOV_STEP_CALLS],
        rep[IOV_STEP_CALLS];
    int timed = parts <= PATH_PARTS;
    int rounds = timed ? IOV_STEP_CALLS : PATH_BIG_ROUNDS;
    uint64_t cps = cycles_per_sec(), smax = 0, rmax = 0, pmax = 0;
    struct _msg_info info;
    pthread_t tid;
    int ok = 1;

    memset(&c, 0, sizeof c);
    c.chid = chid;
    c.parts = parts;
    c.entry = entry;
    c.rounds = rounds;
    for (int i = 0; i < parts * entry; i++)
        c.out[i] = (unsigned char) (i * 3 + 1);
    /* the sender expects its own bytes back, so the server replies with them */
    for (int i = 0; i < parts; i++) {
        SETIOV(&c.sarr[i], c.out + i * entry, entry);
        SETIOV(&c.rarr[i], c.in + i * entry, entry);
        SETIOV(&siov[i], c.out + i * entry, entry);
        SETIOV(&riov[i], sin + i * entry, entry);
    }
    if (pthread_create(&tid, NULL, path_sender, &c) != EOK)
        return -1;
    for (int i = 0; i < rounds && ok; i++) {
        uint64_t t0, t1, t2;
        int rcvid;

        if (!receiver_first)
            path_nap(PATH_NAP_NS + PATH_NAP_NS / 2); /* the sender queues */
        (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_RECEIVE, NULL,
                               &(const uint64_t){STRADDLE_TIMEOUT_NS}, NULL);
        t0 = read_time();
        rcvid = msg_receivev(chid, riov, parts, &info);
        t1 = read_time();
        if (rcvid == -1 || memcmp(sin, c.out, (size_t) (parts * entry)) != 0) {
            ok = 0;
            break;
        }
        t2 = read_time();
        if (msg_replyv(rcvid, 0, siov, parts) == -1) {
            fprintf(stderr, "ipc-roundtrip: path reply round %d: %s\n", i,
                    strerror(errno));
            ok = 0;
        }
        snd[i % IOV_STEP_CALLS] = ticks_to_ns(t1 - c.t_send, cps);
        rcv[i % IOV_STEP_CALLS] = ticks_to_ns(t1 - t0, cps);
        rep[i % IOV_STEP_CALLS] = ticks_to_ns(read_time() - t2, cps);
        memset(sin, 0, sizeof sin);
    }
    (void) pthread_join(tid, NULL);
    ok = ok && c.rc == 0;
    if (timed && receiver_first) {
        /* The delivery includes waking the receiver, which a busy host makes
         * slow whatever the kernel does: a step is judged by what it adds to
         * the same message in one entry.
         */
        uint64_t p99, p50;

        qsort(snd, IOV_STEP_CALLS, sizeof snd[0], cmp_u64);
        p50 = snd[IOV_STEP_CALLS / 2];
        p99 = snd[p99_index(IOV_STEP_CALLS)];
        smax = snd[IOV_STEP_CALLS - 1];
        if (parts == 1) {
            path_base_ns = p99;
        } else {
            ok = ok && p99 < path_base_ns + IOV_STEP_NS_MAX;
        }
        printf(
            "ipc-roundtrip: iov-send %s parts=%d bytes=%d p50-ns=%llu "
            "p99-ns=%llu max-ns=%llu one-entry-p99-ns=%llu\n",
            ok ? "PASS" : "FAIL", parts, parts * entry,
            (unsigned long long) p50, (unsigned long long) p99,
            (unsigned long long) smax, (unsigned long long) path_base_ns);
    } else if (timed) {
        /* Both sides are sorted before either is judged: a short-circuit
         * left the reply side unsorted and its max unset, and printed both.
         */
        int rcv_over = path_p99(rcv, &rmax);
        int rep_over = path_p99(rep, &pmax);

        ok = ok && !rcv_over && !rep_over;
        printf(
            "ipc-roundtrip: iov-receive-reply %s receive-p99-ns=%llu "
            "receive-max-ns=%llu reply-p99-ns=%llu reply-max-ns=%llu\n",
            ok ? "PASS" : "FAIL",
            (unsigned long long) rcv[p99_index(IOV_STEP_CALLS)],
            (unsigned long long) rmax,
            (unsigned long long) rep[p99_index(IOV_STEP_CALLS)],
            (unsigned long long) pmax);
    } else {
        printf("ipc-roundtrip: iov-path-big %s %s parts=%d\n",
               ok ? "PASS" : "FAIL",
               receiver_first ? "receiver-first" : "sender-first", parts);
    }
    return ok ? 0 : -1;
}

/* A thread that sends PATH_SHORT bytes, a short message, and waits for the
 * reply.
 */
static void *iov_short_sender(void *arg)
{
    struct iov_ctx *ctx = arg;
    iov_t siov, riov;
    int coid, r;
    char rep[1];

    ctx->rc = -1;
    coid = connect_attach((uint32_t) 0, 0, ctx->chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1)
        return NULL;
    SETIOV(&siov, ctx->out, PATH_SHORT);
    SETIOV(&riov, rep, sizeof rep);
    r = msg_sendv(coid, &siov, 1, &riov, 1);
    if (r != -1)
        ctx->rc = 0;
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

/* A short message, read into IOV_COPY_PARTS entries of which only the last is
 * not empty (one step, timed), and into IOV_PARTS of them (many restarts, which
 * must move the same bytes, and which an all-empty vector must end with none).
 */
static int check_iov_short(int chid)
{
    static unsigned char out[PATH_SHORT], rd[PATH_SHORT];
    static iov_t ev[IOV_COPY_PARTS], big[IOV_PARTS];
    static struct iov_ctx ctx;
    uint64_t cps = cycles_per_sec(), p99 = 0, max = 0;
    pthread_t tid;
    int rcvid, ok, n;

    ctx = (struct iov_ctx){.chid = chid, .out = out, .in = rd, .rc = -1};
    for (int i = 0; i < PATH_SHORT; i++)
        out[i] = (unsigned char) (i * 5 + 1);
    for (int i = 0; i < IOV_COPY_PARTS - 1; i++)
        SETIOV(&ev[i], rd, 0);
    SETIOV(&ev[IOV_COPY_PARTS - 1], rd, PATH_SHORT);
    for (int i = 0; i < IOV_PARTS - 1; i++)
        SETIOV(&big[i], rd, 0);
    SETIOV(&big[IOV_PARTS - 1], rd, PATH_SHORT);
    if (pthread_create(&tid, NULL, iov_short_sender, &ctx) != EOK)
        return -1;
    if ((rcvid = receive_timed(chid)) == -1)
        return -1;
    ok = iov_copy_time(rcvid, 0, ev, IOV_COPY_PARTS, PATH_SHORT, cps, &p99,
                       &max) == 0 &&
         memcmp(rd, out, PATH_SHORT) == 0;
    memset(rd, 0, sizeof rd);
    n = msg_readv(rcvid, big, IOV_PARTS, 0);
    ok = ok && n == PATH_SHORT && memcmp(rd, out, PATH_SHORT) == 0;
    for (int i = 0; i < IOV_PARTS; i++)
        SETIOV(&big[i], rd, 0);
    n = msg_readv(rcvid, big, IOV_PARTS, 0);
    ok = ok && n == 0;
    if (msg_reply(rcvid, 0, NULL, 0) == -1)
        ok = 0;
    (void) pthread_join(tid, NULL);
    ok = ok && ctx.rc == 0 && p99 < IOV_STEP_NS_MAX;
    printf("ipc-roundtrip: iov-short %s step-p99-ns=%llu step-max-ns=%llu\n",
           ok ? "PASS" : "FAIL", (unsigned long long) p99,
           (unsigned long long) max);
    return ok ? 0 : -1;
}

/* msg_sendv with an array the caller does not own: in kernel space, wrapping
 * past the top of it, or longer than a transfer takes. Bounded by a send
 * timeout, so a kernel that reads the array and queues the send does not hang
 * the run: it fails with ETIMEDOUT, not EFAULT.
 */
static int check_sendv_array(int chid)
{
    static iov_t good[2];
    static unsigned char b[8];
    const uint64_t timeout = STRADDLE_TIMEOUT_NS;
    iov_t *bad[] = {(iov_t *) IOV_KERNEL_VA,
                    (iov_t *) (uintptr_t) (UINTPTR_MAX - sizeof(iov_t))};
    int coid = connect_attach((uint32_t) 0, 0, chid, RTMUX_SIDE_CHANNEL, 0);
    int ok = coid != -1;

    SETIOV(&good[0], b, sizeof b);
    SETIOV(&good[1], b, sizeof b);
    for (size_t i = 0; ok && i < sizeof bad / sizeof bad[0]; i++) {
        for (int side = 0; side < 2; side++) {
            int r;

            (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_SEND, NULL,
                                   &timeout, NULL);
            errno = 0;
            r = side ? msg_sendv(coid, good, 2, bad[i], 2)
                     : msg_sendv(coid, bad[i], 2, good, 2);
            if (r != -1 || errno != EFAULT) {
                fprintf(
                    stderr,
                    "ipc-roundtrip: sendv %s array %zu: %d %s, want EFAULT\n",
                    side ? "reply" : "send", i, r,
                    r == -1 ? strerror(errno) : "");
                ok = 0;
            }
        }
    }
    errno = 0;
    (void) __timer_timeout(CLOCK_MONOTONIC, RTMUX_TIMEOUT_SEND, NULL, &timeout,
                           NULL);
    if (ok && (msg_sendv(coid, good, PATH_TOO_MANY, good, 1) != -1 ||
               errno != EFAULT)) {
        fprintf(stderr,
                "ipc-roundtrip: sendv %d entries: errno %d, want "
                "EFAULT\n",
                PATH_TOO_MANY, errno);
        ok = 0;
    }
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);
    printf("ipc-roundtrip: sendv-array %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : -1;
}

/* The gap between the restarts of one long call, which no timing of whole
 * calls shows. A pair of low-priority threads on one hart exchange messages
 * of GAP_PARTS one-byte entries each way without pause, so that hart is
 * nearly always inside a send, receive or reply: totalling a vector, or
 * copying. A high-priority probe on the same hart waits for a notification
 * that this thread sends from another hart at times of its own choosing. The
 * wake is a reschedule request to the busy hart, not a tick, so the probe
 * runs as soon as the call in progress gives the hart up: the time from the
 * send to the probe's return is the longest the kernel stays in one entry,
 * plus the wake itself. The same wake over a thread that only spins in user
 * space is measured first, for what the wake costs alone: on QEMU its p99 is
 * milliseconds whatever the kernel is doing, far over the budget of a step.
 * So the exchange is judged by what it adds: its median and its p99 must each
 * stay within IOV_STEP_NS_MAX of the wake alone.
 */
#define GAP_PARTS 65536
#define GAP_SAMPLES 200
#define GAP_LOW_PRIORITY 20
#define GAP_PROBE_PRIORITY 50
#define GAP_KICK_PRIORITY 51
#define GAP_BUSY_HART 1U
#define GAP_KICK_HART 0U
#define GAP_CODE_KICK 1
#define GAP_CODE_STOP 2
#define GAP_OP_STOP 0x7e

struct gap_ctx {
    int chid, probe_chid;
    _Atomic uint64_t t_kick;
    _Atomic int woken, stop, spin, failed, rounds;
    uint64_t ns[GAP_SAMPLES];
    unsigned char out[GAP_PARTS], in[GAP_PARTS], srv[GAP_PARTS];
    iov_t sarr[GAP_PARTS], rarr[GAP_PARTS], riov[GAP_PARTS], siov[GAP_PARTS];
};

static int gap_pin(int priority, unsigned cpu)
{
    size_t bytes = sizeof(struct _thread_runmask) + 2U * sizeof(unsigned);
    struct _thread_runmask *m = calloc(1U, bytes);
    unsigned *runmask;
    int rc;

    if (!m || raise_to_rt(priority) != 0) {
        free(m);
        return -1;
    }
    m->size = 1U;
    runmask = (unsigned *) (m + 1);
    runmask[0] = runmask[1] = 1U << cpu; /* run mask, then inherit mask */
    rc = thread_ctl(RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT, m);
    free(m);
    return rc;
}

static void *gap_probe(void *arg)
{
    struct gap_ctx *g = arg;
    uint64_t cps = cycles_per_sec();

    if (gap_pin(GAP_PROBE_PRIORITY, GAP_BUSY_HART) != 0)
        atomic_store(&g->failed, 1);
    for (int n = 0;;) {
        struct _notification note;
        uint64_t now;

        if (msg_receive_notification(g->probe_chid, &note, sizeof note, NULL) !=
            0) {
            atomic_store(&g->failed, 1);
            break;
        }
        now = read_time();
        if (note.code == GAP_CODE_STOP)
            break;
        g->ns[n++ % GAP_SAMPLES] =
            ticks_to_ns(now - atomic_load(&g->t_kick), cps);
        atomic_store(&g->woken, n);
    }
    return NULL;
}

static void *gap_sender(void *arg)
{
    struct gap_ctx *g = arg;
    int coid = connect_attach((uint32_t) 0, 0, g->chid, RTMUX_SIDE_CHANNEL, 0);

    if (coid == -1 || gap_pin(GAP_LOW_PRIORITY, GAP_BUSY_HART) != 0) {
        atomic_store(&g->failed, 1);
        return NULL;
    }
    while (atomic_load(&g->spin) && !atomic_load(&g->stop))
        ; /* the hart is busy in user space: the cost of the wake alone */
    while (!atomic_load(&g->stop)) {
        if (msg_sendv(coid, g->sarr, GAP_PARTS, g->rarr, GAP_PARTS) == -1 ||
            memcmp(g->in, g->out, GAP_PARTS) != 0) {
            atomic_store(&g->failed, 1);
            break;
        }
        atomic_fetch_add(&g->rounds, 1);
    }
    g->out[0] = GAP_OP_STOP;
    (void) msg_send(coid, g->out, 1, NULL, 0);
    (void) __connect_detach((uint64_t) coid);
    return NULL;
}

static void *gap_server(void *arg)
{
    struct gap_ctx *g = arg;

    if (gap_pin(GAP_LOW_PRIORITY, GAP_BUSY_HART) != 0)
        atomic_store(&g->failed, 1);
    for (;;) {
        int rcvid = msg_receivev(g->chid, g->riov, GAP_PARTS, NULL);

        if (rcvid == -1) {
            atomic_store(&g->failed, 1);
            break;
        }
        if (g->srv[0] == GAP_OP_STOP) {
            (void) msg_reply(rcvid, 0, NULL, 0);
            break;
        }
        if (msg_replyv(rcvid, 0, g->siov, GAP_PARTS) == -1) {
            atomic_store(&g->failed, 1);
            break;
        }
    }
    return NULL;
}

/* GAP_SAMPLES wakes of the probe, a few milliseconds apart and never on a
 * period, so they fall anywhere in the busy hart's calls; the median goes to
 * *p50, the p99 to *p99, the slowest to *max.
 */
static int gap_measure(struct gap_ctx *g,
                       int coid,
                       uint64_t *p50,
                       uint64_t *p99,
                       uint64_t *max)
{
    int base = atomic_load(&g->woken);

    for (int i = 0; i < GAP_SAMPLES; i++) {
        struct timespec nap = {.tv_nsec = 1500000L + (i * 7919L) % 1000000L};
        uint64_t t0;

        (void) nanosleep(&nap, NULL);
        atomic_store(&g->t_kick, read_time());
        if (msg_send_notification(coid, GAP_PROBE_PRIORITY, GAP_CODE_KICK, i) !=
            EOK)
            return -1;
        t0 = read_time();
        while (atomic_load(&g->woken) == base + i && !atomic_load(&g->failed) &&
               ticks_to_ns(read_time() - t0, cycles_per_sec()) <
                   STRADDLE_TIMEOUT_NS)
            ;
        if (atomic_load(&g->woken) != base + i + 1)
            return -1;
    }
    qsort(g->ns, GAP_SAMPLES, sizeof g->ns[0], cmp_u64);
    *p50 = g->ns[GAP_SAMPLES / 2];
    *p99 = g->ns[p99_index(GAP_SAMPLES)];
    *max = g->ns[GAP_SAMPLES - 1];
    return 0;
}

struct gap_run {
    int chid, rc;
};

/* Runs in a thread of its own: it pins itself, which the caller must not be. */
static void *gap_control(void *arg)
{
    static struct gap_ctx g;
    uint64_t wake_p50 = 0, wake_p99 = 0, wake_max = 0, p50 = 0, p99 = 0,
             max = 0;
    pthread_t tp, ts, tv;
    struct gap_run *run = arg;
    int coid, ok, rounds;

    memset(&g, 0, sizeof g);
    g.chid = run->chid;
    g.probe_chid = channel_create(0);
    coid = g.probe_chid == -1 ? -1
                              : connect_attach((uint32_t) 0, 0, g.probe_chid,
                                               RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1)
        return NULL;
    for (int i = 0; i < GAP_PARTS; i++) {
        g.out[i] = (unsigned char) (i * 5 + 3);
        if (i == 0)
            g.out[i] = 1; /* never GAP_OP_STOP */
        SETIOV(&g.sarr[i], g.out + i, 1);
        SETIOV(&g.rarr[i], g.in + i, 1);
        SETIOV(&g.riov[i], g.srv + i, 1);
        SETIOV(&g.siov[i], g.out + i, 1);
    }
    atomic_store(&g.spin, 1);
    if (pthread_create(&tp, NULL, gap_probe, &g) != EOK ||
        pthread_create(&tv, NULL, gap_server, &g) != EOK ||
        pthread_create(&ts, NULL, gap_sender, &g) != EOK ||
        gap_pin(GAP_KICK_PRIORITY, GAP_KICK_HART) != 0) {
        /* The run fails and the process exits, its threads with it. */
        fprintf(stderr, "ipc-roundtrip: iov-gap setup failed\n");
        return NULL;
    }
    ok = gap_measure(&g, coid, &wake_p50, &wake_p99, &wake_max) == 0;
    atomic_store(&g.spin, 0); /* the exchange starts */
    ok = ok && gap_measure(&g, coid, &p50, &p99, &max) == 0;
    rounds = atomic_load(&g.rounds); /* exchanges finished under the wakes */
    atomic_store(&g.stop, 1);
    (void) msg_send_notification(coid, GAP_PROBE_PRIORITY, GAP_CODE_STOP, 0);
    (void) pthread_join(ts, NULL);
    (void) pthread_join(tv, NULL);
    (void) pthread_join(tp, NULL);
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(g.probe_chid);
    ok = ok && !atomic_load(&g.failed) && rounds > 0 &&
         p50 < wake_p50 + IOV_STEP_NS_MAX && p99 < wake_p99 + IOV_STEP_NS_MAX;
    printf(
        "ipc-roundtrip: iov-gap %s parts=%d exchanges=%d p50-ns=%llu "
        "p99-ns=%llu "
        "max-ns=%llu wake-only-p50-ns=%llu wake-only-p99-ns=%llu "
        "wake-only-max-ns=%llu\n",
        ok ? "PASS" : "FAIL", GAP_PARTS, rounds, (unsigned long long) p50,
        (unsigned long long) p99, (unsigned long long) max,
        (unsigned long long) wake_p50, (unsigned long long) wake_p99,
        (unsigned long long) wake_max);
    run->rc = ok ? 0 : -1;
    return NULL;
}

static int check_iov_gap(int chid)
{
    struct gap_run run = {.chid = chid, .rc = -1};
    pthread_t tc;

    if (sysconf(_SC_NPROCESSORS_ONLN) < 2) {
        printf("ipc-roundtrip: iov-gap SKIP needs two harts\n");
        return 0;
    }
    if (pthread_create(&tc, NULL, gap_control, &run) != EOK)
        return -1;
    (void) pthread_join(tc, NULL);
    return run.rc;
}

static int check_iov_paths(int chid)
{
    int rc = check_sendv_array(chid);

    rc |= check_iov_short(chid);
    rc |= path_round(chid, 1, 1, PATH_PARTS * PATH_ENTRY);
    rc |= path_round(chid, 1, PATH_PARTS, PATH_ENTRY);
    rc |= path_round(chid, 0, PATH_PARTS, PATH_ENTRY);
    rc |= path_round(chid, 1, PATH_MID, PATH_ENTRY);
    rc |= path_round(chid, 0, PATH_MID, PATH_ENTRY);
    rc |= path_round(chid, 1, PATH_BIG, PATH_ENTRY);
    rc |= path_round(chid, 0, PATH_BIG, PATH_ENTRY);
    rc |= check_iov_gap(chid);
    return rc;
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

    /* The delivery of a waiting receiver's message alone, three ways: one
     * byte (a short message: the wake and nothing else), 512 bytes in one
     * entry, and the same in 32 entries. For telling the cost of the wake
     * from the cost of the copy.
     */
    if (argc == 2 && !strcmp(argv[1], "--iov-send")) {
        int rc;

        chid = channel_create(0);
        if (chid == -1)
            return EXIT_FAILURE;
        rc = path_round(chid, 1, 1, 1);
        rc |= path_round(chid, 1, 1, PATH_PARTS * PATH_ENTRY);
        rc |= path_round(chid, 1, PATH_PARTS, PATH_ENTRY);
        return rc == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    /* One group of checks alone, for a comparison between two kernels. */
    if (argc == 2 &&
        (!strcmp(argv[1], "--iov-paths") || !strcmp(argv[1], "--iov-gap"))) {
        chid = channel_create(0);
        if (chid == -1)
            return EXIT_FAILURE;
        return (!strcmp(argv[1], "--iov-paths") ? check_iov_paths(chid)
                                                : check_iov_gap(chid)) == 0
                   ? EXIT_SUCCESS
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
    if (check_readiov_straddle(chid) != 0 || check_readiov_large(chid) != 0 ||
        check_iov_steps(chid) != 0 || check_iov_bytes(chid) != 0 ||
        check_iov_paths(chid) != 0)
        return EXIT_FAILURE;

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
