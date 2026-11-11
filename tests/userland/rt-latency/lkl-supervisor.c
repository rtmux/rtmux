/*
 * lkl-supervisor - restart a crashed LKL domain server.
 *
 * RTMux has no respawn infrastructure; this is it for the LKL domains. A domain
 * server self-detaches (rtmux_detach) and then publishes its namespace path, so
 * a supervisor cannot waitpid the detached daemon -- instead it watches that
 * published path. The kernel already reclaims a dead process's delegated caps
 * (the lkl-*-crash tests prove the /net and /fs paths vanish on crash), so the
 * path disappearing is the abnormal-exit signal: the supervisor then starts a
 * fresh generation, which re-acquires its own caps and republishes the path.
 * Clients see the path vanish (a transient error) and then reappear.
 *
 * usage: lkl-supervisor WATCHPATH MAXRESTARTS CRASHINJECT|- BIN [ARG...]
 *   e.g. lkl-supervisor /net/ipifc 2 - /sbin/lkl-netd --serve-net
 *
 * Everything from BIN on is the server command, so a multi-arg server
 * (/sbin/lkl-fsd --serve-ext4 IMG ROOT) is supervised the same as a single-arg
 * one. CRASHINJECT is a path to fault a generation, or "-" for none.
 *
 * Prints "lkl-supervisor: gen N up" whenever a generation publishes WATCHPATH
 * and "gen N down" when it vanishes, so a test can gate on a restart having
 * actually happened. Exits 0 after MAXRESTARTS generations have run and the
 * last one is still up; non-zero if a generation never comes up.
 *
 * The optional CRASHINJECT path is a test hook: once generation 0 is up, the
 * supervisor writes a ping line to it to fault that generation (lkl-netd's
 * crash-on-ping path faults when a ping command is written), so the
 * crash->reclaim->restart path can be exercised self-contained. The minimal
 * guest shell has no subshells, so a background "wait-then-crash" client is
 * impractical; this keeps the restart test to a single command.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static uint64_t now_ns(void)
{
    uint64_t t = 0;

    (void) __clock_time(CLOCK_MONOTONIC, NULL, &t);
    return t;
}

#define UP_TIMEOUT_MS 10000 /* a fresh LKL boot can take several seconds */
#define POLL_MS 100
#define SETTLE_MS 500 /* between a failed attempt and the retry */
#define SPAWN_ATTEMPTS 3

static void nap_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000};

    nanosleep(&ts, NULL);
}

static int wait_present(const char *path, long timeout_ms)
{
    for (long waited = 0; waited < timeout_ms; waited += POLL_MS) {
        if (access(path, F_OK) == 0)
            return 0;
        nap_ms(POLL_MS);
    }
    return -1;
}

static void wait_absent(const char *path)
{
    while (access(path, F_OK) == 0)
        nap_ms(POLL_MS);
}

/* Start one server generation and wait for it to publish WATCHPATH.
 *
 * A restart races the previous generation's teardown: lkl-netd detaches its
 * namespace path before it actually faults, so the path can vanish while cap
 * and virtio-device reclaim is still in flight, and a too-eager fresh
 * generation can then fail to acquire the device. Rather than guess a fixed
 * settle delay, retry the spawn: a generation that cannot get its resources
 * exits, so the watchpath stays absent and a retry is safe (never a duplicate
 * live server). The self-detached child needs no reap -- roottask marks
 * detached children NOZOMBIE.
 *
 * Returns 0 once a generation is up, -1 if all attempts fail.
 */
static int start_gen(char *const *server_argv, const char *watch)
{
    for (int attempt = 0; attempt < SPAWN_ATTEMPTS; attempt++) {
        pid_t pid;
        int rc;

        /* A generation that self-detaches is NOZOMBIE, but one that dies before
         * detaching (bad exec, early fault) would linger as our child; reap any
         * such corpse so a retry loop cannot accrete zombies.
         */
        while (waitpid(-1, NULL, WNOHANG) > 0)
            ;
        rc = posix_spawn(&pid, server_argv[0], NULL, NULL, server_argv, environ);

        if (rc != 0) {
            fprintf(stderr, "lkl-supervisor: spawn %s: %s\n", server_argv[0],
                    strerror(rc));
            return -1;
        }
        if (wait_present(watch, UP_TIMEOUT_MS) == 0)
            return 0;
        nap_ms(SETTLE_MS); /* let the still-reclaiming old gen finish, retry */
    }
    return -1;
}

int main(int argc, char **argv)
{
    const char *watch, *crash_inject;
    char *const *server_argv;
    long max_restarts;

    /* WATCHPATH MAXRESTARTS CRASHINJECT|- BIN [ARG...]: everything from BIN on
     * is the server command, so multi-arg servers (lkl-fsd --serve-ext4 IMG
     * ROOT) are supervised the same as single-arg ones (lkl-netd --serve-net).
     */
    if (argc < 5) {
        fprintf(stderr, "usage: lkl-supervisor WATCHPATH MAXRESTARTS "
                        "CRASHINJECT|- BIN [ARG...]\n");
        return 2;
    }
    watch = argv[1];
    crash_inject = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
    server_argv = &argv[4];

    {
        char *end;

        errno = 0;
        max_restarts = strtol(argv[2], &end, 10);
        if (errno != 0 || end == argv[2] || *end != '\0' || max_restarts < 0) {
            fprintf(stderr, "lkl-supervisor: bad MAXRESTARTS '%s'\n", argv[2]);
            return 2;
        }
    }

    uint64_t down_ns = 0;

    for (long gen = 0; gen <= max_restarts; gen++) {
        if (start_gen(server_argv, watch) != 0) {
            fprintf(stderr, "lkl-supervisor: gen %ld never published %s\n", gen,
                    watch);
            return 1;
        }
        printf("lkl-supervisor: gen %ld up\n", gen);
        /* Restart latency: from the previous generation going down to this one
         * republishing WATCHPATH. Dominated by the fresh LKL boot, so it is a
         * coarse recovery bound, not a tight metric -- recorded, not promised.
         */
        if (gen > 0 && down_ns != 0)
            printf("lkl-supervisor: gen %ld restart_ms=%llu\n", gen,
                   (unsigned long long) ((now_ns() - down_ns) / 1000000));
        fflush(stdout);

        if (gen == max_restarts)
            break; /* leave the last generation serving */

        /* Test hook: fault this generation so the restart path runs unattended.
         * A crash-on-ping lkl-netd faults when a ping line is written to its
         * /net/ping path, so write one; the write errors as the server dies,
         * which is expected and ignored. Every non-final generation is faulted,
         * so MAXRESTARTS > 1 keeps working rather than blocking on a live gen.
         */
        if (crash_inject) {
            int fd = open(crash_inject, O_WRONLY);

            if (fd >= 0) {
                (void) write(fd, "10.0.2.2 1\n", 11);
                close(fd);
            }
        }

        wait_absent(watch);
        down_ns = now_ns();
        printf("lkl-supervisor: gen %ld down, restarting\n", gen);
        fflush(stdout);
    }

    printf("lkl-supervisor: done, %ld generation(s) supervised\n",
           max_restarts + 1);
    return 0;
}
