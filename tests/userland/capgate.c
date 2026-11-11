/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Capability-gated global channel conformance.
 *
 * A global channel resolves by chid alone, so before this gate any process
 * could connect to any registered global channel by naming its integer chid.
 * RTMUX_FLG_CHF_CAP_GATED closes that hole: the creator mints a gating cap over
 * the channel, and a connect is admitted only if the caller holds a live cap
 * naming the channel object. Public (required_cap == 0) channels stay open.
 */

#include <errno.h>
#include <stddef.h>
#include <pthread.h>
#include <fcntl.h>
#include <sched.h>
#include <semaphore.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/_pthreadtypes.h>
#include <sys/iomsg.h>
#include <sys/mman.h>
#include <sys/procmsg.h>
#include <sys/rtmux-core.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../../kernel/include/private/priv-calls.h"
#include "test.h"

/* Try to connect to a gated global channel from a fresh child that holds no
 * gating cap. Invoked as: syscall_testing --connect-gated <chid>. Exit 0 iff
 * the connect is refused with EPERM (the expected denial), nonzero otherwise.
 */
int capgate_child_connect(const char *chid_arg)
{
    int chid = (int) strtol(chid_arg, NULL, 0);
    int coid;

    errno = 0;
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    if (coid == -1 && errno == EPERM)
        return EXIT_SUCCESS;
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);
    return EXIT_FAILURE;
}

static void test_capgate_flag_requires_global(void)
{
    int rc;

    errno = 0;
    rc = channel_create(RTMUX_FLG_CHF_CAP_GATED);
    check(rc == -1 && errno == EINVAL,
          "cap-gate flag without global is rejected",
          "RTMUX_FLG_CHF_CAP_GATED only applies to a global channel");
}

static void test_capgate_public_still_open(void)
{
    int chid, coid;

    chid = channel_create(RTMUX_FLG_CHF_GLOBAL);
    if (chid == -1) {
        test_fail("public global channel_create", strerror(errno));
        return;
    }
    check((unsigned) chid & RTMUX_GLOBAL_CHANNEL,
          "public global channel carries the global chid bit", "");

    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    check(coid != -1, "public global channel connects without a cap",
          strerror(errno));
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
}

static void test_capgate_holder_and_stranger(void)
{
    uint64_t cap = 0;
    int chid, coid;
    char chid_str[24];
    char *argv[] = {"syscall_testing", "--connect-gated", chid_str, NULL};
    pid_t pid;
    int status, rc;

    chid = channel_create_cap(0, &cap);
    if (chid == -1) {
        test_fail("gated global channel_create_cap", strerror(errno));
        return;
    }
    check(((unsigned) chid & RTMUX_GLOBAL_CHANNEL) && cap != 0,
          "gated create returns a global chid and a nonzero gating cap", "");

    /* The creator holds the minted cap over the channel object, so the same
     * object-identity gate a delegated client hits admits it.
     */
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    check(coid != -1, "cap holder connects to the gated channel",
          strerror(errno));
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);

    /* A fresh process inherits no caps, so it must be refused. */
    snprintf(chid_str, sizeof chid_str, "%d", chid);
    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
    if (rc != EOK) {
        test_fail("gated-connect child fixture", strerror(rc));
        (void) channel_destroy(chid);
        return;
    }
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == EXIT_SUCCESS,
          "process without the cap is refused (EPERM)",
          "name-only connect to a gated global channel must fail");

    (void) channel_destroy(chid);
}

static void capgate_thread_stub(union sigval v)
{
    (void) v;
}

/* Creating a thread in another process is reserved to holders of
 * RTMUX_PRIV_CAP_PROCESS (roottask and any server the bootstrap manifest grants
 * it), no longer to the literal roottask pid. This test process is an ordinary
 * child with priv_caps 0, so the kernel must refuse the cross-process create
 * with EPERM. Target pid 1 exists, so the call reaches the authority guard
 * rather than failing the process lookup.
 */
static void test_capgate_cross_process_thread_denied(void)
{
    int rc;

    errno = 0;
    rc = __thread_create(1, (void *) capgate_thread_stub, NULL, NULL);
    check(rc == -1 && errno == EPERM,
          "cross-process thread create is refused without the capability",
          strerror(errno));
}

static void test_manifest_priv_caps_denied(void)
{
    static const struct {
        int id;
        const char *name;
    } cases[] = {
#define PRIV_DENY_CASE(token) {.id = __KSYS_PRIV_##token, .name = #token}
        PRIV_DENY_CASE(MMAP_PHYS_USER),
        PRIV_DENY_CASE(TRACE),
        PRIV_DENY_CASE(CLOCK_START),
        PRIV_DENY_CASE(PROCESS_CREATE),
        PRIV_DENY_CASE(THREAD_CTL),
        PRIV_DENY_CASE(PROC_REGISTER_SYSCHAN),
        PRIV_DENY_CASE(CHANNEL_HAS_RECEIVERS),
        PRIV_DENY_CASE(DEBUG_INJECT),
        PRIV_DENY_CASE(SOUL_STATS),
#undef PRIV_DENY_CASE
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        long rc = syscall_priv(cases[i].id, NULL);
        char detail[96];

        snprintf(detail, sizeof(detail), "%s missing cap must be EPERM",
                 cases[i].name);
        check(rc == -EPERM, detail,
              "unlisted process must not pass private-cap dispatch");
    }
}

/* rtmux_spawn_cap (SPAWN_PROC_CAP) delivers a process capability over the child
 * to the parent in the spawn reply. The child fixture just exits; the parent
 * only checks it received a nonzero cap handle and the spawn still returns the
 * child pid. A plain spawn (no flag), exercised elsewhere, is unaffected.
 */
static void test_proc_cap_spawn(void)
{
    char *argv[] = {"syscall_testing", "--proc-cap-child", NULL};
    int fd_map[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    struct inheritance inherit = {.flags = 0};
    uint64_t cap = 0;
    pid_t pid;
    int status;

    pid = rtmux_spawn_cap("/bin/syscall_testing", 3, fd_map, &inherit, argv,
                          NULL, &cap);
    check(pid > 0, "spawn with SPAWN_PROC_CAP returns a child pid",
          strerror(errno));
    check(cap != 0, "parent receives a process cap over the child",
          "the spawn reply must carry a minted process cap");
    /* Reap by the returned pid: this also proves the pid is the real child, not
     * a value clamped by the reply payload.
     */
    if (pid > 0)
        check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == EXIT_SUCCESS,
              "proc-cap child reaps by the reply-carried pid",
              "the pid returned with the cap must identify the real child");
}

/* A held process cap authorizes cross-process thread create. The parent spawns
 * a child with rtmux_spawn_cap (so it holds a MAKE_THREAD cap over the child),
 * keeps the child alive on a named semaphore, then creates a thread in it. A
 * deliberately tiny stack makes thread_create fail with EINVAL after the
 * authority gate, so no real cross-process thread runs; the point is that the
 * result is EINVAL (past the gate) and not EPERM (denied). The mirror deny
 * case, a caller with neither the priv cap nor a process cap, is
 * test_capgate_cross_process_thread_denied above.
 */
static void test_proc_cap_thread_authorized(void)
{
    char *argv[] = {"syscall_testing", "--proc-cap-hold", NULL};
    int fd_map[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO};
    struct inheritance inherit = {.flags = 0};
    /* The kernel copies the attr in this caller's context, so a stack object
     * is fine: the target's address space is never consulted.
     */
    struct _thread_attr tattr = {.stackaddr = (void *) 1, .stacksize = 1};
    uint64_t cap = 0;
    sem_t *ready, *release;
    pid_t pid;
    int status, rc;

    /* Clear any stale state a crashed prior run may have left, so the counts
     * start at zero and the ready/release handshake is not pre-satisfied.
     */
    (void) sem_unlink("/proccap-ready");
    (void) sem_unlink("/proccap-release");
    ready = sem_open("/proccap-ready", O_CREAT, 0600, 0);
    release = sem_open("/proccap-release", O_CREAT, 0600, 0);
    if (ready == SEM_FAILED || release == SEM_FAILED) {
        test_fail("proc-cap thread sem_open", strerror(errno));
        return;
    }

    pid = rtmux_spawn_cap("/bin/syscall_testing", 3, fd_map, &inherit, argv,
                          NULL, &cap);
    if (pid <= 0 || cap == 0) {
        test_fail("proc-cap thread spawn", strerror(errno));
        sem_close(ready);
        sem_close(release);
        sem_unlink("/proccap-ready");
        sem_unlink("/proccap-release");
        return;
    }

    /* Wait until the child is alive and blocked, so it is a live target. */
    sem_wait(ready);

    errno = 0;
    rc = __thread_create((int) pid, (void *) capgate_thread_stub, NULL, &tattr);
    {
        char why[64];

        /* Past the gate, and the errno is the caller's attr verdict: the tiny
         * stack is EINVAL.
         */
        snprintf(why, sizeof why, "rc=%d errno=%d (want -1/EINVAL)", rc, errno);
        check(rc == -1 && errno == EINVAL,
              "process-cap holder passes the cross-process thread-create gate",
              why);
    }

    /* The unsupported-scope bit outranks the stack check, in any target. */
    tattr.flags = PTHREAD_SCOPE_PROCESS;
    errno = 0;
    rc = __thread_create((int) pid, (void *) capgate_thread_stub, NULL, &tattr);
    {
        char why[64];

        snprintf(why, sizeof why, "rc=%d errno=%d (want -1/ENOTSUP)", rc, errno);
        check(rc == -1 && errno == ENOTSUP,
              "cross-process thread-create validates the caller's attr",
              why);
    }

    sem_post(release);
    (void) waitpid(pid, &status, 0);
    sem_close(ready);
    sem_close(release);
    sem_unlink("/proccap-ready");
    sem_unlink("/proccap-release");
}

/* Reopen another process's descriptor through /proc/<pid>/fd/<n>. libc has no
 * key for a foreign connection, so roottask (servers/roottask/path/procfs.c)
 * mints one for the opener's thread and returns it in a message link. Only a
 * root-euid caller is admitted; the reopen cannot exceed the source's access;
 * two reopens in a row by one thread both work (each key is minted for one
 * link and the link is followed before the next open can start).
 */
static void test_procfs_foreign_reopen(void)
{
    char *argv[] = {"syscall_testing", "--proc-cap-hold", NULL};
    struct inheritance inherit = {.flags = 0};
    uint64_t cap = 0;
    sem_t *ready, *release;
    char path[64], buf[8];
    uid_t olduid = geteuid();
    int fd_map[4], nullfd, fd1, fd2, status;
    pid_t pid;

    nullfd = open("/dev/null", O_RDWR);
    (void) sem_unlink("/proccap-ready");
    (void) sem_unlink("/proccap-release");
    ready = sem_open("/proccap-ready", O_CREAT, 0600, 0);
    release = sem_open("/proccap-release", O_CREAT, 0600, 0);
    if (nullfd == -1 || ready == SEM_FAILED || release == SEM_FAILED) {
        test_fail("procfs foreign reopen setup", strerror(errno));
        return;
    }
    fd_map[0] = STDIN_FILENO;
    fd_map[1] = STDOUT_FILENO;
    fd_map[2] = STDERR_FILENO;
    fd_map[3] = nullfd;
    pid = rtmux_spawn_cap("/bin/syscall_testing", 4, fd_map, &inherit, argv,
                          NULL, &cap);
    if (pid <= 0) {
        test_fail("procfs foreign reopen spawn", strerror(errno));
        goto out;
    }
    sem_wait(ready);

    snprintf(path, sizeof path, "/proc/%d/fd/3", (int) pid);
    fd1 = open(path, O_RDWR);
    fd2 = open(path, O_RDONLY);
    check(fd1 != -1 && fd2 != -1 && write(fd1, "x", 1) == 1 &&
              read(fd2, buf, sizeof buf) == 0,
          "procfs_foreign_reopen_root: root reopens a foreign fd, twice in turn",
          strerror(errno));
    if (fd1 != -1)
        close(fd1);
    if (fd2 != -1)
        close(fd2);

    snprintf(path, sizeof path, "/proc/%d/fd/99", (int) pid);
    errno = 0;
    check(open(path, O_RDONLY) == -1 && errno == EBADF,
          "procfs_foreign_reopen_badfd: a closed foreign fd is EBADF", "");

    snprintf(path, sizeof path, "/proc/%d/fd/3", (int) pid);
    if (seteuid(1000) != 0) {
        test_fail("procfs_foreign_reopen_nonroot", "seteuid(1000)");
    } else {
        errno = 0;
        fd1 = open(path, O_RDONLY);
        check(fd1 == -1 && errno == EACCES,
              "procfs_foreign_reopen_nonroot: a non-root caller is EACCES", "");
        if (fd1 != -1)
            close(fd1);
        (void) seteuid(olduid);
    }

    sem_post(release);
    (void) waitpid(pid, &status, 0);
out:
    close(nullfd);
    sem_close(ready);
    sem_close(release);
    sem_unlink("/proccap-ready");
    sem_unlink("/proccap-release");
}

/* ---- _IO_DUP / _IO_OPENFD carry no ambient authority --------------------
 *
 * A dup or openfd of another process's handle is allowed only for the sender's
 * own connection, for roottask, or with a key the proc/path service minted for
 * that thread over those exact bytes. Matching uids (or a root sender) is not
 * possession of the handle.
 */

/* Model of the first (FNV-style) keygen, which was a bijection of its seed:
 * one (bytes, key) pair recovered the seed, after which keys for any other
 * target could be forged. Kept to prove the current keyed PRF resists that.
 */
static uint32_t old_keygen(const void *p, size_t n, uint32_t seed)
{
    const unsigned char *cp = p;
    uint32_t h = 0x811c9dc5u ^ seed;

    while (n--)
        h = (h ^ *cp++) * 0x01000193u;
    h ^= h >> 15;
    h *= 0x2c1b3c6du;
    h ^= h >> 12;
    return h ? h : 1;
}

static uint32_t inv_mul(uint32_t a)
{
    uint32_t x = a;

    for (int i = 0; i < 5; i++)
        x *= 2 - a * x;
    return x;
}

static uint32_t inv_xorshift(uint32_t y, int s)
{
    uint32_t x = y;

    for (int i = 0; i < 32 / s + 1; i++)
        x = y ^ (x >> s);
    return x;
}

static uint32_t old_recover_seed(const void *p, size_t n, uint32_t key)
{
    const unsigned char *cp = p;
    uint32_t h = inv_xorshift(key, 12);

    h *= inv_mul(0x2c1b3c6du);
    h = inv_xorshift(h, 15);
    while (n--)
        h = (h * inv_mul(0x01000193u)) ^ cp[n];
    return h ^ 0x811c9dc5u;
}

/* Send a dup and an openfd of the victim's handle with wrong keys (none, a
 * guess, and the checksum under thread key 0); each must be refused EACCES.
 * Returns nonzero if any was admitted.
 */
static int dup_attack_one(pid_t spid,
                          int chid,
                          int32_t scoid,
                          int32_t coid,
                          pid_t vpid,
                          int openfd_only)
{
    io_dup_t d;
    io_openfd_t o;
    int fd, bad = 0;

    fd = connect_attach(0, spid, chid, RTMUX_SIDE_CHANNEL, 0);
    if (fd == -1)
        return 1;
    for (int n = 0; n < (openfd_only ? 0 : 3); n++) {
        memset(&d, 0, sizeof d);
        d.i.type = _IO_DUP;
        d.i.combine_len = sizeof d.i;
        d.i.info.pid = vpid;
        d.i.info.chid = chid;
        d.i.info.scoid = scoid;
        d.i.info.coid = coid;
        d.i.key = n == 0 ? 0
                         : n == 1 ? 0x5eed1234u
                                  : old_keygen(&d, offsetof(struct _io_dup, key), 0);
        errno = 0;
        if (msg_send(fd, &d.i, sizeof d.i, 0, 0) != -1 || errno != EACCES) {
            printf("dup attack %d not refused (errno %d)\n", n, errno);
            bad = 1;
        }
    }
    memset(&o, 0, sizeof o);
    o.i.type = _IO_OPENFD;
    o.i.combine_len = sizeof o.i;
    o.i.ioflag = _IO_FLAG_RD | _IO_FLAG_WR;
    o.i.info.pid = vpid;
    o.i.info.chid = chid;
    o.i.info.scoid = scoid;
    o.i.info.coid = coid;
    errno = 0;
    if (msg_send(fd, &o.i, sizeof o.i, 0, 0) != -1 || errno != EACCES) {
        printf("openfd_foreign_pid not refused (errno %d)\n", errno);
        bad = 1;
    }
    (void) __connect_detach((uint64_t) fd);
    return bad;
}

/* Attacker fixture. argv: --dup-attack <same_uid|root|openfd> <victim pid> then
 * {server-pid chid scoid coid}... for each victim handle. It waits for the
 * parent to drop its own euid (so the old uid rules would have matched), then
 * exits 0 iff every attempt was refused.
 */
int capgate_child_dup_attack(int argc, char **argv)
{
    char c;
    int bad = 0;

    if (argc < 8 || (argc - 4) % 4 != 0)
        return EXIT_FAILURE;
    if (read(STDIN_FILENO, &c, 1) != 1)
        return EXIT_FAILURE;
    if (!strcmp(argv[2], "same_uid") && seteuid(1000) != 0)
        return EXIT_FAILURE;
    for (int i = 4; i + 3 < argc; i += 4)
        bad |= dup_attack_one((pid_t) atoi(argv[i]), atoi(argv[i + 1]),
                              (int32_t) atoi(argv[i + 2]),
                              (int32_t) atoi(argv[i + 3]),
                              (pid_t) atoi(argv[3]), !strcmp(argv[2], "openfd"));
    return bad ? EXIT_FAILURE : EXIT_SUCCESS;
}

/* Replay fixture: after loading, resend the loader's captured dup (bytes and
 * key, passed as hex) on inherited fd 3. The key was minted for the loader
 * thread, so this thread must be refused.
 */
int capgate_child_dup_replay(const char *hex)
{
    io_dup_t d;
    unsigned char *b = (unsigned char *) &d;

    if (strlen(hex) != 2 * sizeof d)
        return EXIT_FAILURE;
    for (size_t i = 0; i < sizeof d; i++) {
        char t[3] = {hex[2 * i], hex[2 * i + 1], 0};

        b[i] = (unsigned char) strtoul(t, NULL, 16);
    }
    errno = 0;
    if (msg_send(3, &d.i, sizeof d.i, 0, 0) == -1 && errno == EACCES)
        return EXIT_SUCCESS;
    return EXIT_FAILURE;
}

static int dup_victim_args(char **av, int n, int fd, char (*buf)[24])
{
    struct _server_info si;

    if (connect_server_info(0, (uint64_t) fd, &si) != fd)
        return n;
    snprintf(buf[0], 24, "%d", si.pid);
    snprintf(buf[1], 24, "%d", si.chid);
    snprintf(buf[2], 24, "%d", si.scoid);
    snprintf(buf[3], 24, "%d", si.coid);
    for (int i = 0; i < 4; i++)
        av[n++] = buf[i];
    return n;
}

/* Spawn the attacker as role (same_uid | root) against the victim fds while
 * the parent's euid is 1000, then restore.
 */
static void dup_attack_role(const char *role,
                            const char *name,
                            int fd1,
                            int fd2)
{
    char *argv[16] = {"syscall_testing", "--dup-attack", (char *) role};
    char vpid[24], b1[4][24], b2[4][24];
    int pfd[2], fd_map[3], n, status;
    struct inheritance inherit = {.flags = 0};
    uint64_t cap = 0;
    uid_t olduid = geteuid();
    pid_t pid;

    if (pipe(pfd) != 0) {
        test_fail(name, "pipe");
        return;
    }
    snprintf(vpid, sizeof vpid, "%d", (int) getpid());
    argv[3] = vpid;
    n = dup_victim_args(argv, 4, fd1, b1);
    n = dup_victim_args(argv, n, fd2, b2);
    argv[n] = NULL;
    fd_map[0] = pfd[0];
    fd_map[1] = STDOUT_FILENO;
    fd_map[2] = STDERR_FILENO;
    pid = rtmux_spawn_cap("/bin/syscall_testing", 3, fd_map, &inherit, argv,
                          NULL, &cap);
    if (pid <= 0) {
        test_fail(name, "spawn attacker");
        close(pfd[0]);
        close(pfd[1]);
        return;
    }
    if (seteuid(1000) != 0)
        test_fail(name, "seteuid(1000) for the victim");
    (void) write(pfd[1], "g", 1);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == EXIT_SUCCESS,
          name, "dup/openfd of another process's handle must be EACCES");
    (void) seteuid(olduid);
    close(pfd[0]);
    close(pfd[1]);
}

struct dupsrv {
    int chid;
    io_dup_t cap;
    volatile int have, altered_rejected, verify_rc, calc_rc, alt_rc;
    volatile int forge_rejected, efault_rc, einval_rc;
    volatile int forge_rc;
    volatile unsigned forge_k2;
    volatile unsigned alt_k2;
    volatile int retire_rc;
    volatile unsigned retire_k2;
    /* Abandoned-load watch: after replying to the loader's dup, keep verifying
     * the same key against the same rcvid. OK (key accepted) turning into a
     * mismatch proves the loader's revoke scrambled its thread secret; the
     * thread vanishing first (ESRCH) proves nothing.
     */
    volatile int watch, watch_ok, watch_revoked, watch_gone;
};

/* Stand-in server for a parent fd: verifies each dup's key with msg_key_data
 * exactly as _ioserv_access does, from a process holding no IPC cap.
 */
static void *dupsrv_thread(void *arg)
{
    struct dupsrv *s = arg;

    for (;;) {
        io_dup_t m, alt;
        iov_t iov;
        unsigned k2 = ~0u, nk;
        int rcvid, rc, ok, watching;

        memset(&m, 0, sizeof m);
        if ((rcvid = msg_receive(s->chid, &m, sizeof m, NULL)) <= 0)
            continue;
        if (m.i.type == 0xFFFF) {
            (void) msg_error(rcvid, EOK);
            return NULL;
        }
        SETIOV(&iov, &m, offsetof(struct _io_dup, key));
        rc = msg_key_data_r(rcvid, RTMUX_KEYDATA_VERIFY, m.i.key, &k2, &iov, 1);
        ok = rc == EOK && k2 == 0;
        if (!s->have && ok) {
            s->verify_rc = rc;
            s->cap = m;
            alt = m;
            alt.i.info.coid ^= 1;
            SETIOV(&iov, &alt, offsetof(struct _io_dup, key));
            k2 = 0;
            s->alt_rc = msg_key_data_r(rcvid, RTMUX_KEYDATA_VERIFY, m.i.key,
                                       &k2, &iov, 1);
            s->alt_k2 = k2;
            s->altered_rejected = s->alt_rc == EOK && k2 != 0;
            s->calc_rc = msg_key_data_r(rcvid, RTMUX_KEYDATA_CALCULATE, 0, &nk,
                                        &iov, 1);
            /* Attack: invert the old hash from this pair to get a "seed",
             * then forge keys for a different target (and the same one).
             */
            {
                uint32_t seed = old_recover_seed(
                    &m, offsetof(struct _io_dup, key), m.i.key);
                unsigned f2 = 0;
                io_dup_t forged = alt;

                forged.i.key = old_keygen(&alt, offsetof(struct _io_dup, key),
                                          seed);
                SETIOV(&iov, &alt, offsetof(struct _io_dup, key));
                s->forge_rc = msg_key_data_r(rcvid, RTMUX_KEYDATA_VERIFY,
                                             forged.i.key, &f2, &iov, 1);
                s->forge_k2 = f2;
                s->forge_rejected = s->forge_rc == EOK && f2 != 0;
            }
            /* An unmapped part and an oversized part count fail closed. */
            {
                void *pg = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANON, NOFD, 0);
                iov_t bad[5];
                unsigned f4 = 0;

                if (pg != MAP_FAILED) {
                    munmap(pg, 4096);
                    SETIOV(&bad[0], pg, 64);
                    s->efault_rc = msg_key_data_r(
                        rcvid, RTMUX_KEYDATA_VERIFY, 1, &f4, bad, 1);
                }
                for (int b = 0; b < 5; b++)
                    SETIOV(&bad[b], &m, 8);
                s->einval_rc = msg_key_data_r(rcvid, RTMUX_KEYDATA_VERIFY, 1,
                                              &f4, bad, 5);
            }
            /* KEYDATA_MAX_FAILS wrong keys retire the secret: the correct
             * key then no longer verifies. The budget is per client thread
             * and is already partly spent: the altered-bytes and forged-key
             * probes above each counted one wrong key (the EFAULT and
             * EINVAL probes fail before counting), so the secret retires
             * before this loop ends. Looping the full constant keeps the
             * test independent of how many earlier probes count.
             */
            {
                unsigned r2 = 0;

                SETIOV(&iov, &m, offsetof(struct _io_dup, key));
                for (int b = 0; b < KEYDATA_MAX_FAILS; b++)
                    (void) msg_key_data_r(rcvid, RTMUX_KEYDATA_VERIFY,
                                          m.i.key ^ 0x5a5a5a5au, &r2, &iov, 1);
                r2 = 0;
                s->retire_rc = msg_key_data_r(rcvid, RTMUX_KEYDATA_VERIFY,
                                              m.i.key, &r2, &iov, 1);
                s->retire_k2 = r2;
            }
            s->have = 1;
        }
        /* The key just verified, so any later mismatch is the revoke. */
        watching = ok && s->watch;
        if (watching) {
            s->watch = 0;
            s->watch_ok = 1;
        }
        (void) msg_error(rcvid, ok ? EOK : EACCES);
        if (watching) {
            SETIOV(&iov, &m, offsetof(struct _io_dup, key));
            for (int n = 0; n < 50000 && !s->watch_revoked && !s->watch_gone;
                 n++) {
                unsigned w = 0;

                rc = msg_key_data_r(rcvid, RTMUX_KEYDATA_VERIFY, m.i.key, &w,
                                    &iov, 1);
                if (rc != EOK)
                    s->watch_gone = 1;
                else if (w != 0)
                    s->watch_revoked = 1;
                sched_yield();
            }
        }
    }
}

static void test_dup_key_flow(void)
{
    struct dupsrv s;
    pthread_t th;
    char *argv_a[] = {"syscall_testing", "--proc-cap-child", NULL};
    char hex[2 * sizeof(io_dup_t) + 1];
    char *argv_b[] = {"syscall_testing", "--dup-replay-child", hex, NULL};
    int fd_map[4] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, -1};
    struct inheritance inherit = {.flags = 0};
    uint64_t cap = 0;
    io_dup_t f;
    int coid, status;
    pid_t pid;

    memset(&s, 0, sizeof s);
    if ((s.chid = channel_create(0)) == -1 ||
        (coid = connect_attach(0, getpid(), s.chid, 10, 0)) == -1 ||
        pthread_create(&th, NULL, dupsrv_thread, &s) != 0) {
        test_fail("dup key flow setup", strerror(errno));
        return;
    }
    fd_map[3] = coid;

    /* The loader sends the child's inherited dup with a minted key. */
    pid = rtmux_spawn_cap("/bin/syscall_testing", 4, fd_map, &inherit, argv_a,
                          NULL, &cap);
    check(pid > 0 && s.have, "spawn inheritance dup carries a verifiable key",
          "the loader's _IO_DUP must verify against its own thread key");
    if (pid > 0)
        (void) waitpid(pid, &status, 0);
    check(s.have && s.verify_rc == EOK,
          "msg_key_data VERIFY without the IPC cap returns EOK", "");
    snprintf(hex, sizeof hex,
             "calc=%d alt=%d efault=%d einval=%d forge=%d/%x", s.calc_rc,
             s.alt_rc, s.efault_rc, s.einval_rc, s.forge_rc, s.forge_k2);
    check(s.have && s.calc_rc == -EPERM,
          "msg_key_data CALCULATE without the IPC cap returns EPERM", hex);
    check(s.have && s.altered_rejected,
          "dup_altered_bytes: valid key over changed io_dup_t is refused", hex);

    check(s.have && s.retire_rc == EOK && s.retire_k2 != 0,
          "msg_key_data secret is retired after KEYDATA_MAX_FAILS wrong keys",
          "the correct key must stop verifying once the budget is spent");
    check(s.have && s.forge_rejected,
          "dup key forged by inverting the old hash from a minted pair is "
          "refused",
          hex);
    check(s.have && s.efault_rc == -EFAULT && s.einval_rc == -EINVAL,
          "msg_key_data unmapped part is refused, too many parts EINVAL", hex);

    if (s.have) {
        /* The key belongs to the loader thread; this thread must not pass. */
        errno = 0;
        check(msg_send(coid, &s.cap.i, sizeof s.cap.i, 0, 0) == -1 &&
                  errno == EACCES,
              "dup_wrong_thread: key minted for another thread is refused", "");
        f = s.cap;
        f.i.info.coid += 1;
        f.i.key = old_keygen(&f, offsetof(struct _io_dup, key), 0);
        errno = 0;
        check(msg_send(coid, &f.i, sizeof f.i, 0, 0) == -1 && errno == EACCES,
              "dup_unset_key: checksum under key 0 never verifies", "");

        for (size_t i = 0; i < sizeof s.cap; i++)
            snprintf(hex + 2 * i, 3, "%02x", ((unsigned char *) &s.cap)[i]);
        pid = rtmux_spawn_cap("/bin/syscall_testing", 4, fd_map, &inherit,
                              argv_b, NULL, &cap);
        check(pid > 0 && waitpid(pid, &status, 0) == pid &&
                  WIFEXITED(status) && WEXITSTATUS(status) == EXIT_SUCCESS,
              "dup_replay_after_load: captured key replayed post-load refused",
              "");
    }

    /* Abandoned load: fd 3 is inherited (the loader mints a key and sends the
     * dup), then the next listed fd is dead, so loader_fd fails and the loader
     * revokes its key (_PROC_SPAWN_FD_REVOKE) before exiting. The spawn must
     * fail with EBADF, and the proc service must stay usable afterwards.
     */
    {
        int bad_map[5] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, coid,
                          0x7fff};

        errno = 0;
        s.watch = 1;
        pid = rtmux_spawn("/bin/syscall_testing", 5, bad_map, &inherit, argv_a,
                          NULL);
        check(pid == -1 && errno == EBADF,
              "spawn_fd_abandon: a dead listed fd fails the spawn with EBADF",
              "the loader must abandon loading and revoke its key");
        for (int n = 0; n < 200 && !s.watch_revoked && !s.watch_gone; n++) {
            struct timespec ts = {0, 1000000};

            nanosleep(&ts, NULL);
        }
        snprintf(hex, sizeof hex, "ok=%d revoked=%d gone=%d", s.watch_ok,
                 s.watch_revoked, s.watch_gone);
        check(s.watch_ok && s.watch_revoked,
              "spawn_fd_abandon: the loader's revoke scrambles its minted key",
              hex);
        pid = rtmux_spawn_cap("/bin/syscall_testing", 4, fd_map, &inherit,
                              argv_a, NULL, &cap);
        check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
                  WEXITSTATUS(status) == EXIT_SUCCESS,
              "spawn_fd_abandon: a spawn after the abandoned load still works",
              "");
    }

    memset(&f, 0, sizeof f);
    f.i.type = 0xFFFF;
    (void) msg_send(coid, &f.i, sizeof f.i, 0, 0);
    pthread_join(th, NULL);
    (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(s.chid);
}

/* The key service answers only a child's own loader: an ordinary process asking
 * for a key (or fd info) about its own or any other pid is refused.
 */
static void test_spawn_fd_not_loader(void)
{
    proc_spawn_fd_t m;
    struct _proc_spawn_fd_info ent;
    int32_t list = 0;
    iov_t siov[2], riov[2];
    int rc;

    memset(&m, 0, sizeof m);
    m.i.type = _PROC_SPAWN;
    m.i.subtype = _PROC_SPAWN_FD;
    m.i.flags = _PROC_SPAWN_FD_LIST;
    m.i.nfds = 1;
    m.i.ppid = getppid();
    SETIOV(siov + 0, &m.i, sizeof m.i);
    SETIOV(siov + 1, &list, sizeof list);
    SETIOV(riov + 0, &m.o, sizeof m.o);
    SETIOV(riov + 1, &ent, sizeof ent);
    errno = 0;
    rc = msg_sendv(ROOTTASK_COID, siov, 2, riov, 2);
    check(rc == -1 && errno == EACCES,
          "spawn_fd_not_loader: _PROC_SPAWN_FD from a non-loader is EACCES",
          "the key service must not mint or reveal fd info for non-loaders");
}

static void test_dup_ambient_authority(void)
{
    test_spawn_fd_not_loader();
    int nullfd = open("/dev/null", O_RDWR);
    int pfd[2];

    if (nullfd == -1 || pipe(pfd) != 0) {
        test_fail("dup ambient authority fixtures", strerror(errno));
        return;
    }
    /* The parent's euid becomes 1000 while a root attacker, and one that
     * dropped to 1000, each guess the parent's handles: both matched the old
     * uid rules. A third attacker sends only a foreign-pid _IO_OPENFD.
     */
    dup_attack_role("same_uid", "dup_guessed_scoid_same_uid", nullfd, pfd[0]);
    dup_attack_role("root", "dup_guessed_scoid_root", nullfd, pfd[0]);
    dup_attack_role("openfd", "openfd_foreign_pid", nullfd, pfd[0]);
    close(nullfd);
    close(pfd[0]);
    close(pfd[1]);
    test_dup_key_flow();
}

/* Native-cap reply parsing is opt-in by the client (RTMUX_TCTL_NATIVE_REPLY),
 * never inferred from the reply bytes. An in-process server replies with a
 * well-formed native header (nxfers > 0) naming an endpoint cap it holds. The
 * kernel rewrites xfers[0].id to the installed handle when it parses the reply,
 * so "bytes unchanged" proves the reply was treated as opaque data.
 */
#define NATREPLY_MAX 4096
#define NATREPLY_STOP 0xFFFFU

struct natreply_req {
    uint32_t len;
    uint32_t kind; /* 0: native header, 1: exec-crash bytes, 2: bad-version header */
};

static const unsigned char natreply_exec_prefix[8] = {0x13, 0x01, 0xb8, 0x00,
                                                      0x17, 0x00, 0x07, 0x00};

static void natreply_fill(unsigned char *buf,
                          uint32_t len,
                          uint32_t kind,
                          uint64_t cap)
{
    struct _io_native_hdr hdr;
    uint32_t n;

    memset(buf, 0xAB, len);
    if (kind == 1) {
        memcpy(buf, natreply_exec_prefix, len < 8 ? len : 8);
        return;
    }
    memset(&hdr, 0, sizeof hdr);
    hdr.msg.type = _IO_MSG;
    hdr.msg.protocol = _IOMSG_RTMUX_NATIVE;
    hdr.version = kind == 2 ? _IO_NATIVE_VERSION + 1 : _IO_NATIVE_VERSION;
    hdr.nxfers = 1;
    hdr.xfers[0].id = cap;
    hdr.xfers[0].rights = RTMUX_CAP_ENDPOINT_SEND;
    n = len < sizeof hdr ? len : (uint32_t) sizeof hdr;
    memcpy(buf, &hdr, n);
}

static uint64_t natreply_cap;
static int natreply_ctl_fail; /* thread_ctl(NATIVE_REPLY) calls that returned != 0 */

static void *natreply_server(void *arg)
{
    int chid = (int) (intptr_t) arg;
    static unsigned char out[NATREPLY_MAX];

    for (;;) {
        struct natreply_req req;
        struct _msg_info info;
        int rcvid = msg_receive(chid, &req, sizeof req, &info);

        if (rcvid == -1)
            return NULL;
        if (req.len == NATREPLY_STOP) {
            (void) msg_reply(rcvid, 0, NULL, 0);
            return NULL;
        }
        natreply_fill(out, req.len, req.kind, natreply_cap);
        (void) msg_reply(rcvid, 0, out, (int) req.len);
    }
}

/* Send one request; the reply lands in rbuf. */
static int natreply_call(int coid,
                         uint32_t len,
                         uint32_t kind,
                         int armed,
                         unsigned char *rbuf)
{
    struct natreply_req req = {.len = len, .kind = kind};
    int n;

    memset(rbuf, 0, NATREPLY_MAX);
    if (armed && thread_ctl(RTMUX_TCTL_NATIVE_REPLY, (void *) 1) != 0)
        natreply_ctl_fail++;
    n = msg_send(coid, &req, sizeof req, rbuf, len);
    if (armed && thread_ctl(RTMUX_TCTL_NATIVE_REPLY, (void *) 0) != 0)
        natreply_ctl_fail++;
    return n;
}

static void test_native_reply_opt_in(void)
{
    static const uint32_t sizes[] = {8, 100, sizeof(struct _io_native_hdr), 512,
                                     NATREPLY_MAX};
    static unsigned char want[NATREPLY_MAX], got[NATREPLY_MAX];
    struct _server_info info;
    pthread_t th;
    int chid, coid, n;
    char name[96];

    chid = channel_create(0);
    if (chid == -1) {
        test_fail("native-reply channel_create", strerror(errno));
        return;
    }
    coid = connect_attach(0, getpid(), chid, RTMUX_SIDE_CHANNEL, 0);
    memset(&info, 0, sizeof info);
    if (coid == -1 || connect_server_info(0, (uint64_t) coid, &info) != coid ||
        info.endpoint_cap == 0) {
        test_fail("native-reply connect", strerror(errno));
        goto out;
    }
    natreply_cap = info.endpoint_cap;
    if (pthread_create(&th, NULL, natreply_server, (void *) (intptr_t) chid)) {
        test_fail("native-reply server thread", "pthread_create");
        goto out;
    }

    /* (1) A non-opted-in client must receive a valid-looking native header
     * with nxfers > 0 byte for byte, at every reply size (short-buffer and
     * long-message paths), and gain no capability.
     */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint32_t len = sizes[i];

        natreply_fill(want, len, 0, natreply_cap);
        n = natreply_call(coid, len, 0, 0, got);
        snprintf(name, sizeof name,
                 "native-looking reply to a plain request is data (%u bytes)",
                 len);
        check(n == 0 && memcmp(got, want, len) == 0, name,
              "reply bytes were altered: caps were parsed from the payload");
    }

    /* The earlier regression: a read reply that opens `13 01 b8 00 17 00 07
     * 00` (two instruction words) must arrive unchanged.
     */
    natreply_fill(want, 512, 1, natreply_cap);
    n = natreply_call(coid, 512, 1, 0, got);
    check(n == 0 && memcmp(got, want, 512) == 0,
          "512-byte reply opening 13 01 b8 00 17 00 07 00 arrives unchanged",
          "exec-image byte pattern was mangled");

    /* (2) An opted-in client still gets the caps: the kernel installs the cap
     * and rewrites the transfer id to the receiver's handle.
     */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint32_t len = sizes[i];
        const struct _io_native_hdr *hdr = (const void *) got;

        if (len < sizeof(struct _io_native_hdr))
            continue;
        n = natreply_call(coid, len, 0, 1, got);
        snprintf(name, sizeof name,
                 "opted-in client still receives the native cap (%u bytes)",
                 len);
        check(n == 0 && hdr->nxfers == 1 && hdr->xfers[0].id != 0 &&
                  hdr->xfers[0].id != natreply_cap,
              name, "the transferred cap id was not installed/rewritten");
    }

    /* (3) An opted-in reply shorter than the header is plain data. */
    for (unsigned i = 0; i < 2; i++) {
        uint32_t len = sizes[i];

        natreply_fill(want, len, 0, natreply_cap);
        n = natreply_call(coid, len, 0, 1, got);
        snprintf(name, sizeof name,
                 "opted-in reply shorter than the header is data (%u bytes)",
                 len);
        check(n == 0 && memcmp(got, want, len) == 0, name,
              "short reply must pass through unparsed");
    }

    /* (4) An opted-in client whose reply opens like a native header but has a
     * wrong version is data: the look-alike must arrive byte for byte with no
     * cap installed (the kernel would rewrite xfers[0].id if it parsed it).
     */
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint32_t len = sizes[i];

        if (len < sizeof(struct _io_native_hdr))
            continue;
        natreply_fill(want, len, 2, natreply_cap);
        n = natreply_call(coid, len, 2, 1, got);
        snprintf(name, sizeof name,
                 "opted-in header look-alike with a bad version is data (%u bytes)",
                 len);
        check(n == 0 && memcmp(got, want, len) == 0, name,
              "a wrong-version native header was parsed or altered");
    }
    check(natreply_ctl_fail == 0,
          "thread_ctl(RTMUX_TCTL_NATIVE_REPLY) set and clear both return 0",
          "the opt-in call failed, so the opted-in cases above proved nothing");

    /* The opt-in is scoped to one request: it is clear again afterwards. */
    natreply_fill(want, 512, 0, natreply_cap);
    n = natreply_call(coid, 512, 0, 0, got);
    check(n == 0 && memcmp(got, want, 512) == 0,
          "opt-in does not leak into the next request",
          "a later plain request had its reply parsed");

    (void) natreply_call(coid, NATREPLY_STOP, 0, 0, got);
    pthread_join(th, NULL);
out:
    if (coid != -1)
        (void) __connect_detach((uint64_t) coid);
    (void) channel_destroy(chid);
}

void test_capgate(void)
{
    test_capgate_flag_requires_global();
    test_capgate_public_still_open();
    test_capgate_holder_and_stranger();
    test_capgate_cross_process_thread_denied();
    test_proc_cap_spawn();
    test_native_reply_opt_in();
    test_proc_cap_thread_authorized();
    test_manifest_priv_caps_denied();
    test_dup_ambient_authority();
    test_procfs_foreign_reopen();
}
