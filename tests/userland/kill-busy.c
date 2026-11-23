/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * kill-busy — a SIGKILL must end a process promptly even while roottask is
 * working for it.
 *
 * The victim maps and unmaps a 2 MiB shared object in a loop. The parent sends
 * SIGKILL 20 to 400 ms after the spawn returned and requires kill to succeed
 * and waitpid to report the signal within a second. The first kills land while
 * the victim is still starting, before its libc has registered the channel
 * signals are delivered on: roottask used to answer those with ESRCH and drop
 * the signal, so the victim ran for ever. The later ones land while a thread of
 * the victim is reply-blocked on roottask in the middle of a memory request.
 * kill_busy runs that alone; kill_busy_under_spawn runs it while a second
 * thread of the parent spawns and reaps trivial children, the arrangement in
 * which the kill was first seen not to take effect for more than ten minutes.
 *
 * kill_stuck_system_thread and kill_no_system_channel cover the two victims
 * whose own libc cannot act on a signal: one whose system thread is inside a
 * handler that does not return, and one that is linked without crt0 and so
 * never registered a system channel (tests/userland/no-syschan.c). Roottask
 * carries a SIGKILL out itself, so both must be reaped within a second too.
 *
 * kill_while_resolving kills victims whose threads look paths up without a
 * pause, so that roottask often takes a victim's lookup up after the victim is
 * gone.
 *
 * kill_during_map (--map-kill, debug-inject kernels) kills a victim in
 * the middle of a MAP that roottask is zeroing pages for.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/memmsg.h>
#include <sys/rtmux-core.h>

#include "test.h"

#define KILL_BUSY_BYTES (2UL * 1024 * 1024)
#define KILL_BUSY_ROUNDS 6
#define KILL_BUSY_LIMIT_NS 1000000000ULL
#define KILL_BUSY_GIVE_UP_NS 5000000000ULL
/* Long enough for a victim that was not killed to end by itself. */
#define KILL_BUSY_OUTLIVE_NS 25000000000ULL
#define KILL_RESOLVE_THREADS 6
#define KILL_RESOLVE_ROUNDS 24
#define KILL_MAP_BYTES (8UL * 1024 * 1024)
#define KILL_MAP_SLOW "2000/2048" /* 2 ms more for each of its 2048 pages */

static uint64_t kb_now_ns(void)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000000000ULL + (uint64_t) ts.tv_nsec;
}

/* --busy-map-child: map and unmap a 2 MiB object for ever. */
int kill_busy_child(void)
{
    struct flock space;
    char shm[32];
    int fd;

    (void) snprintf(shm, sizeof shm, "/kill-busy-%d", (int) getpid());
    fd = shm_open(shm, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0)
        return 4;
    (void) shm_unlink(shm);
    memset(&space, 0, sizeof space);
    space.l_whence = SEEK_SET;
    space.l_len = (off_t) KILL_BUSY_BYTES;
    if (fcntl(fd, F_ALLOCSP, &space) == -1)
        return 4;
    for (;;) {
        unsigned char *v = mmap(NULL, KILL_BUSY_BYTES, PROT_READ | PROT_WRITE,
                                MAP_SHARED, fd, 0);

        if (v == MAP_FAILED)
            return 5;
        v[0] = 1;
        v[KILL_BUSY_BYTES - 1] = 1;
        if (munmap(v, KILL_BUSY_BYTES) != 0)
            return 5;
    }
}

/* When each kill is sent, in ms after the spawn returned: three while the
 * victim is still starting and three once it is in its loop.
 */
static const long kb_delay_ms[KILL_BUSY_ROUNDS] = {20, 37, 54, 200, 300, 400};

/* Poll for child pid for at most limit_ns; 1 with *status set if it was reaped.
 * Every wait in this file is bounded, so a child that does not end fails its
 * case instead of hanging the suite.
 */
static int kb_reap(pid_t pid, int *status, uint64_t limit_ns)
{
    uint64_t t0 = kb_now_ns();

    for (;;) {
        if (waitpid(pid, status, WNOHANG) == pid)
            return 1;
        if (kb_now_ns() - t0 > limit_ns)
            return 0;
        sleep_ms(2);
    }
}

static _Atomic int kb_stop;

static void *kb_spawner(void *arg)
{
    unsigned long *failed = arg;
    char *argv[] = {"syscall_testing", "--exit-code", "0", NULL};

    while (!kb_stop) {
        pid_t pid;
        int status;

        if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) !=
            0) {
            (*failed)++;
        } else if (!kb_reap(pid, &status, KILL_BUSY_GIVE_UP_NS)) {
            /* Do not leave it behind: end it and collect it. */
            (*failed)++;
            (void) kill(pid, SIGKILL);
            (void) kb_reap(pid, &status, KILL_BUSY_GIVE_UP_NS);
        } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            (*failed)++;
        }
    }
    return NULL;
}

/* Kill KILL_BUSY_ROUNDS victims; report the slowest kill-to-reap time and how
 * many did not die of SIGKILL.
 */
static void kb_kill_rounds(const char *name)
{
    char *argv[] = {"syscall_testing", "--busy-map-child", NULL};
    uint64_t worst = 0;
    int wrong = 0, last = 0, kill_errno = 0, refused = 0, stuck = 0;
    char detail[200];

    for (int i = 0; i < KILL_BUSY_ROUNDS; i++) {
        uint64_t t0, elapsed;
        pid_t pid;
        int status = 0, refused_now, reaped = 0;

        int rc =
            posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);

        if (rc != 0) {
            (void) snprintf(detail, sizeof detail,
                            "spawn of victim %d failed: errno %d", i, rc);
            test_fail(name, detail);
            return;
        }
        sleep_ms(kb_delay_ms[i]);
        t0 = kb_now_ns();
        errno = 0;
        refused_now = kill(pid, SIGKILL) != 0;
        if (refused_now) {
            if (!kill_errno)
                kill_errno = errno;
            refused++;
        }

        /* Poll for the victim, for at most KILL_BUSY_GIVE_UP_NS, so a lost
         * signal fails the case instead of hanging it. A refused kill is sent
         * again every 300 ms: the victim never exits by itself and must not
         * outlive the test.
         */
        for (unsigned polls = 0;; polls++) {
            if (waitpid(pid, &status, WNOHANG) == pid) {
                reaped = 1;
                break;
            }
            if (kb_now_ns() - t0 > KILL_BUSY_GIVE_UP_NS)
                break;
            if (refused_now && polls % 150 == 149)
                (void) kill(pid, SIGKILL);
            sleep_ms(2);
        }
        elapsed = kb_now_ns() - t0;
        if (elapsed > worst)
            worst = elapsed;
        if (!reaped) {
            stuck++;
            (void) kill(pid, SIGKILL);
        } else if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
            wrong++;
            last = status;
        }
    }
    (void) snprintf(detail, sizeof detail,
                    "%d kills: slowest kill-to-reap %llu ms (limit %llu), %d "
                    "not SIGKILL (last status 0x%x), %d refused (errno %d), %d "
                    "never reaped",
                    KILL_BUSY_ROUNDS, (unsigned long long) (worst / 1000000),
                    (unsigned long long) (KILL_BUSY_LIMIT_NS / 1000000), wrong,
                    last, refused, kill_errno, stuck);
    printf("  INFO  %s: %s\n", name, detail);
    check(
        wrong == 0 && refused == 0 && stuck == 0 && worst <= KILL_BUSY_LIMIT_NS,
        name, detail);
}

static void test_kill_busy_under_spawn(void)
{
    const char *name = "kill_busy_under_spawn";
    unsigned long spawn_failed = 0;
    pthread_t spawner;

    kb_stop = 0;
    if (pthread_create(&spawner, NULL, kb_spawner, &spawn_failed) != 0) {
        test_fail(name, "pthread_create failed");
        return;
    }
    kb_kill_rounds(name);
    kb_stop = 1;
    (void) pthread_join(spawner, NULL);
    check(spawn_failed == 0, "kill_busy_under_spawn: spawns beside the kills",
          "a spawn beside the kills failed");
}

/* --stuck-handler-child: catch SIGUSR1 with a handler that does not return for
 * 8 s. Handlers run on libc's system thread, the thread that would also act on
 * any later signal, so while this one runs the process's libc is deaf. The
 * handler says it has started through a shared page named after the pid, and
 * the process ends by itself (exit 7 from the handler, 8 from main after 20 s)
 * so that a kernel that cannot kill it does not keep it for ever.
 */
static volatile unsigned char *kb_stuck_flag;

static void kb_stuck_name(char *buf, size_t len, pid_t pid)
{
    (void) snprintf(buf, len, "/kill-stuck-%d", (int) pid);
}

static void kb_stuck_handler(int signo)
{
    (void) signo;
    kb_stuck_flag[0] = 1;
    sleep_ms(8000);
    _exit(7);
}

int kill_stuck_handler_child(void)
{
    struct sigaction sa;
    struct flock space;
    char shm[32];
    int fd;

    kb_stuck_name(shm, sizeof shm, getpid());
    fd = shm_open(shm, O_RDWR | O_CREAT, 0600);
    memset(&space, 0, sizeof space);
    space.l_whence = SEEK_SET;
    space.l_len = 4096;
    if (fd < 0 || fcntl(fd, F_ALLOCSP, &space) == -1)
        return 4;
    kb_stuck_flag = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (kb_stuck_flag == MAP_FAILED)
        return 4;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = kb_stuck_handler;
    if (sigaction(SIGUSR1, &sa, NULL) != 0)
        return 4;
    kb_stuck_flag[1] = 1; /* the handler is installed */
    sleep_ms(20000);
    return 8;
}

/* Wait, for at most 3 s, until byte `which` of the victim's shared page is set:
 * 1 once its handler is installed, 0 once the handler is running.
 */
static int kb_stuck_wait(pid_t pid, int which)
{
    char shm[32];
    uint64_t t0 = kb_now_ns();

    kb_stuck_name(shm, sizeof shm, pid);
    while (kb_now_ns() - t0 < 3000000000ULL) {
        int fd = shm_open(shm, O_RDONLY, 0);
        struct stat st;

        /* The victim creates the object before it sizes it: a page mapped
         * while the object is empty lies past its end, and reading it faults.
         */
        if (fd >= 0 && fstat(fd, &st) == 0 && st.st_size >= 4096) {
            const unsigned char *p =
                mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
            int set = p != MAP_FAILED && p[which];

            if (p != MAP_FAILED)
                (void) munmap((void *) p, 4096);
            (void) close(fd);
            if (set)
                return 1;
        } else if (fd >= 0) {
            (void) close(fd);
        }
        sleep_ms(5);
    }
    return 0;
}

/* SIGKILL must end a process whatever its own libc is doing. Spawn `path`; for
 * the stuck victim, send SIGUSR1 and wait until its handler is running, so the
 * system thread is known to be occupied; then send SIGKILL and require the
 * child reaped as killed by it within the limit.
 */
static void kb_kill_deaf(const char *name,
                         const char *path,
                         char **argv,
                         int stuck)
{
    uint64_t t0, elapsed;
    pid_t pid;
    int status = 0, reaped, rc, kill_rc;
    char detail[200], shm[32];

    rc = posix_spawn(&pid, path, NULL, NULL, argv, NULL);
    if (rc != 0) {
        (void) snprintf(detail, sizeof detail, "spawn of %s failed: errno %d",
                        path, rc);
        test_fail(name, detail);
        return;
    }
    if (stuck) {
        int ready = kb_stuck_wait(pid, 1) && kill(pid, SIGUSR1) == 0 &&
                    kb_stuck_wait(pid, 0);

        kb_stuck_name(shm, sizeof shm, pid);
        (void) shm_unlink(shm);
        if (!ready) {
            (void) kill(pid, SIGKILL);
            (void) kb_reap(pid, &status, KILL_BUSY_OUTLIVE_NS);
            test_fail(name, "the victim's handler never started");
            return;
        }
    } else {
        sleep_ms(50); /* it is loaded when the spawn returns; let it spin */
    }
    t0 = kb_now_ns();
    kill_rc = kill(pid, SIGKILL) != 0 ? errno : 0;
    reaped = kb_reap(pid, &status, KILL_BUSY_GIVE_UP_NS);
    elapsed = kb_now_ns() - t0;
    (void) snprintf(detail, sizeof detail,
                    "kill errno %d, %s after %llu ms (limit %llu), status 0x%x",
                    kill_rc, reaped ? "reaped" : "not reaped",
                    (unsigned long long) (elapsed / 1000000),
                    (unsigned long long) (KILL_BUSY_LIMIT_NS / 1000000),
                    status);
    printf("  INFO  %s: %s\n", name, detail);
    check(kill_rc == 0 && reaped && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGKILL && elapsed <= KILL_BUSY_LIMIT_NS,
          name, detail);

    /* A victim that outlived the limit ends by itself within about 20 s; wait
     * that long so it does not stay behind as a zombie.
     */
    if (!reaped)
        (void) kb_reap(pid, &status, KILL_BUSY_OUTLIVE_NS);
}

static void test_kill_deaf(void)
{
    char *stuck[] = {"syscall_testing", "--stuck-handler-child", NULL};
    char *bare[] = {"no-syschan", NULL};

    kb_kill_deaf("kill_stuck_system_thread", "/bin/syscall_testing", stuck, 1);
    kb_kill_deaf("kill_no_system_channel", "/bin/no-syschan", bare, 0);
}

/* --resolve-loop-child: look paths up from several threads for ever. Each
 * lookup is a request roottask works on with the process's root and cwd. The
 * threads run below the parent's priority, and roottask serves a request at
 * its sender's: on a hart they share, the kill the parent sends, and the
 * teardown after it, overtake a lookup roottask has received and not yet
 * started on.
 */
static void *kb_resolve_loop(void *arg)
{
    struct sched_param sp;
    int policy;

    (void) arg;
    if (pthread_getschedparam(pthread_self(), &policy, &sp) == 0 &&
        sp.sched_priority > 2)
        (void) pthread_setschedprio(pthread_self(), sp.sched_priority - 2);
    for (;;)
        (void) access("/bin/syscall_testing", F_OK);
    return NULL;
}

int kill_resolve_child(void)
{
    pthread_t t;

    for (int i = 0; i < KILL_RESOLVE_THREADS - 1; i++)
        if (pthread_create(&t, NULL, kb_resolve_loop, NULL) != 0)
            return 4;
    (void) kb_resolve_loop(NULL);
    return 5;
}

/* Kill a process while its threads have path lookups in roottask. A lookup
 * that roottask takes up after the process is gone must find its namespace
 * gone, not freed: roottask used to read the process's root after the exit
 * had detached it, and stopped on the NULL node. Every victim must be reaped
 * as SIGKILL within the usual limit.
 */
static void test_kill_resolving(void)
{
    const char *name = "kill_while_resolving";
    char *argv[] = {"syscall_testing", "--resolve-loop-child", NULL};
    int wrong = 0, stuck = 0, failed = 0, last = 0;
    char detail[160];

    for (int i = 0; i < KILL_RESOLVE_ROUNDS; i++) {
        pid_t pid;
        int status = 0;

        if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv,
                        NULL) != 0) {
            failed++;
            continue;
        }
        sleep_ms(5 + (i * 7) % 31);
        (void) kill(pid, SIGKILL);
        /* Leave the dead victim unreaped for a while: until it is reaped, its
         * slot is still there for a late lookup to lock.
         */
        sleep_ms(20);
        if (!kb_reap(pid, &status, KILL_BUSY_LIMIT_NS)) {
            stuck++;
            (void) kill(pid, SIGKILL);
            (void) kb_reap(pid, &status, KILL_BUSY_GIVE_UP_NS);
        } else if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
            wrong++;
            last = status;
        }
    }
    (void) snprintf(detail, sizeof detail,
                    "%d kills: %d spawns failed, %d not reaped in %llu ms, %d "
                    "not SIGKILL (last status 0x%x)",
                    KILL_RESOLVE_ROUNDS, failed, stuck,
                    (unsigned long long) (KILL_BUSY_LIMIT_NS / 1000000), wrong,
                    last);
    printf("  INFO  %s: %s\n", name, detail);
    check(failed == 0 && stuck == 0 && wrong == 0, name, detail);
}

/* --thread-loop-child: create short threads without a pause, for ever. */
static void *kb_thread_noop(void *arg)
{
    return arg;
}

int kill_thread_loop_child(void)
{
    for (;;) {
        pthread_t t;

        if (pthread_create(&t, NULL, kb_thread_noop, NULL) == 0)
            (void) pthread_detach(t);
    }
}

/* Kill a process while it creates threads. The exit marks the process and then
 * destroys its threads in a pass of its own; a thread created after that pass
 * used to be started anyway, in an address space the exit then freed, where it
 * faulted for ever and the process was never reaped. Every victim must be
 * reaped as SIGKILL within the usual limit.
 */
static void test_kill_creating(void)
{
    const char *name = "kill_while_creating_threads";
    char *argv[] = {"syscall_testing", "--thread-loop-child", NULL};
    int wrong = 0, stuck = 0, failed = 0, last = 0;
    char detail[160];

    for (int i = 0; i < KILL_RESOLVE_ROUNDS; i++) {
        pid_t pid;
        int status = 0;

        if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv,
                        NULL) != 0) {
            failed++;
            continue;
        }
        sleep_ms(5 + (i * 7) % 31);
        (void) kill(pid, SIGKILL);
        if (!kb_reap(pid, &status, KILL_BUSY_LIMIT_NS)) {
            stuck++;
            (void) kill(pid, SIGKILL);
            (void) kb_reap(pid, &status, KILL_BUSY_GIVE_UP_NS);
        } else if (!(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL)) {
            wrong++;
            last = status;
        }
    }
    (void) snprintf(detail, sizeof detail,
                    "%d kills: %d spawns failed, %d not reaped in %llu ms, %d "
                    "not SIGKILL (last status 0x%x)",
                    KILL_RESOLVE_ROUNDS, failed, stuck,
                    (unsigned long long) (KILL_BUSY_LIMIT_NS / 1000000), wrong,
                    last);
    printf("  INFO  %s: %s\n", name, detail);
    check(failed == 0 && stuck == 0 && wrong == 0, name, detail);
}

void test_kill_busy(void)
{
    kb_kill_rounds("kill_busy");
    test_kill_deaf();
    test_kill_busy_under_spawn();
    test_kill_resolving();
    test_kill_creating();
}

/* --big-map-child: say so on fd 4, wait for a byte on fd 3, then make one MAP
 * of KILL_MAP_BYTES whose pages roottask zeroes before it answers (preload is
 * the whole length), and wait for the kill.
 */
int kill_map_child(void)
{
    mem_map_t msg;
    char go;

    if (write(4, "R", 1) != 1 || read(3, &go, 1) != 1)
        return 4;
    memset(&msg, 0, sizeof msg);
    msg.i.type = _MEM_MAP;
    msg.i.len = KILL_MAP_BYTES;
    msg.i.prot = PROT_READ | PROT_WRITE;
    msg.i.flags = MAP_PRIVATE | MAP_ANON;
    msg.i.fd = NOFD;
    msg.i.preload = KILL_MAP_BYTES;
    (void) msg_sendnc(VM_COID, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o);
    for (;;)
        (void) sleep(1000);
}

/* A SIGKILL that lands while roottask zeroes the pages of a large MAP for the
 * victim must still reap the victim within the usual limit. The MAP holds the
 * victim's address space lock, which the exit's unmap needs; zeroing on to
 * the end of the run kept the exit waiting for as long as the MAP took.
 * The debug fault-injection call makes each page of that MAP take 2 ms longer,
 * so the MAP lasts about 4 s and the kill lands in the middle of it.
 */
static void test_kill_during_map(void)
{
    const char *name = "kill_during_map";
    char *argv[] = {"syscall_testing", "--big-map-child", NULL};
    posix_spawn_file_actions_t fa;
    int to_child[2], from_child[2], status = 0, reaped;
    char pidarg[16], ready = 0, detail[128];
    uint64_t t0, waited;
    pid_t pid;

    if (inject_run("alloc-status", NULL, NULL) != 0) {
        test_skip(name, "fault injection unavailable");
        return;
    }
    if (pipe(to_child) != 0 || pipe(from_child) != 0 ||
        posix_spawn_file_actions_init(&fa) != 0 ||
        posix_spawn_file_actions_adddup2(&fa, to_child[0], 3) != 0 ||
        posix_spawn_file_actions_adddup2(&fa, from_child[1], 4) != 0 ||
        posix_spawn(&pid, "/bin/syscall_testing", &fa, NULL, argv, NULL) != 0) {
        test_fail(name, "pipe or spawn failed");
        return;
    }
    (void) posix_spawn_file_actions_destroy(&fa);
    (void) close(to_child[0]);
    (void) close(from_child[1]);
    (void) snprintf(pidarg, sizeof pidarg, "%d", (int) pid);
    if (read(from_child[0], &ready, 1) != 1 ||
        inject_run("slow-kpage-zero", pidarg, KILL_MAP_SLOW) != 0 ||
        write(to_child[1], "G", 1) != 1 ||
        inject_run("site-wait", "kpage-zero", NULL) != 0) {
        test_fail(name, "the victim's MAP never reached the slowed zeroing");
        (void) kill(pid, SIGKILL);
        (void) kb_reap(pid, &status, KILL_BUSY_OUTLIVE_NS);
        goto out;
    }
    sleep_ms(100); /* well inside the MAP */
    t0 = kb_now_ns();
    (void) kill(pid, SIGKILL);
    reaped = kb_reap(pid, &status, KILL_BUSY_LIMIT_NS);
    waited = kb_now_ns() - t0;
    if (!reaped) {
        (void) inject_run("site-release", "kpage-zero", NULL);
        reaped = kb_reap(pid, &status, KILL_BUSY_OUTLIVE_NS);
        waited = kb_now_ns() - t0;
    }
    (void) snprintf(detail, sizeof detail,
                    "reaped=%d after %llu ms (limit %llu), status 0x%x", reaped,
                    (unsigned long long) (waited / 1000000),
                    (unsigned long long) (KILL_BUSY_LIMIT_NS / 1000000), status);
    printf("  INFO  %s: %s\n", name, detail);
    check(reaped && waited <= KILL_BUSY_LIMIT_NS && WIFSIGNALED(status) &&
              WTERMSIG(status) == SIGKILL,
          name, detail);
out:
    (void) inject_run("site-release", "kpage-zero", NULL);
    (void) close(to_child[1]);
    (void) close(from_child[0]);
}

/* --map-kill: the debug-inject stage's case alone. */
void test_kill_map(void)
{
    test_kill_during_map();
}
