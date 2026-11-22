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
 * Not tested here, and not true yet: a SIGKILL also has to end a process whose
 * libc cannot act on it, one whose system thread is blocked or that never
 * registered a system channel at all. A signal is delivered by the target's own
 * libc, so such a process survives a SIGKILL today; ending it needs a path in
 * roottask that does not ask the victim.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include "test.h"

#define KILL_BUSY_BYTES (2UL * 1024 * 1024)
#define KILL_BUSY_ROUNDS 6
#define KILL_BUSY_LIMIT_NS 1000000000ULL
#define KILL_BUSY_GIVE_UP_NS 5000000000ULL

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

static _Atomic int kb_stop;

static void *kb_spawner(void *arg)
{
    unsigned long *failed = arg;
    char *argv[] = {"syscall_testing", "--exit-code", "0", NULL};

    while (!kb_stop) {
        pid_t pid;
        int status;

        if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) !=
                0 ||
            waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 0)
            (*failed)++;
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
                            "spawn of victim %d failed: %s", i, strerror(rc));
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

void test_kill_busy(void)
{
    kb_kill_rounds("kill_busy");
    test_kill_busy_under_spawn();
}
