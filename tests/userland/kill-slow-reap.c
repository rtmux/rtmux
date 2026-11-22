/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * kill-slow-reap — a process that is slow to reap must not hold up other
 * requests about it.
 *
 * Roottask ends a process by marking its exit and destroying its threads. A
 * thread that a message transfer holds cannot be destroyed yet, and the pass
 * is made again every millisecond until it can. Those passes used to run with
 * the process lock held, so every other request that names the process (a
 * kill, a wait, a getpgid) waited for the transfer as well.
 *
 * The victim, a child of this binary, naps until it is killed. The debug fault
 * injection makes roottask's pass at destroying its threads report, for
 * SLOW_REAP_HOLD_MS, that one is still held, as a pass does while a message
 * transfer holds a thread. A second thread sends the victim SIGKILL meanwhile,
 * and the main thread then times kill(victim, 0), a request that takes the
 * victim's lock. It must answer well inside the hold. The victim must still be
 * reaped as SIGKILL once the hold is over.
 *
 * Needs CONFIG_DEBUG_INJECT (skipped without it).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "test.h"

#define SLOW_REAP_HOLD_MS 500
#define SLOW_REAP_HOLD_ARG "500"
#define SLOW_REAP_PROBE_LIMIT_MS 125 /* a quarter of the hold */
#define SLOW_REAP_WAIT_MS 5000

static uint64_t sr_now_ms(void)
{
    struct timespec ts;

    (void) clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t) ts.tv_sec * 1000ULL + (uint64_t) ts.tv_nsec / 1000000ULL;
}

/* --slow-reap-child: nap until killed. */
int slow_reap_child(void)
{
    for (;;)
        sleep_ms(5);
}

struct sr_call {
    int id; /* the pid to kill */
    atomic_int done;
    int rc;
    uint64_t ms; /* how long the call took */
};

static void *sr_killer(void *raw)
{
    struct sr_call *c = raw;
    uint64_t t0 = sr_now_ms();

    c->rc = kill((pid_t) c->id, SIGKILL) == 0 ? 0 : errno;
    c->ms = sr_now_ms() - t0;
    atomic_store(&c->done, 1);
    return NULL;
}

static int sr_wait(atomic_int *flag)
{
    for (int i = 0; i < SLOW_REAP_WAIT_MS && !atomic_load(flag); i++)
        sleep_ms(1);
    return atomic_load(flag) != 0;
}

int test_kill_slow_reap(void)
{
    static const char name[] = "kill_slow_reap";
    char *argv[] = {"syscall_testing", "--slow-reap-child", NULL};
    char victim[16], detail[192];
    struct sr_call killer = {0};
    pthread_t tk;
    uint64_t t0, probe_ms;
    int status = 0, reaped = 0;
    pid_t pid;

    if (inject_run("alloc-status", NULL, NULL) != 0)
        return TC_EXIT_SKIP;
    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) !=
        0) {
        test_fail(name, "spawn of the victim failed");
        return 0;
    }
    snprintf(victim, sizeof victim, "%d", (int) pid);

    /* The victim's threads are destroyed at once; the hold makes roottask
     * take it that one is not, and pass again, until the hold ends.
     */
    if (inject_run("hold-exit-destroy", victim, SLOW_REAP_HOLD_ARG) != 0) {
        test_fail(name, "arm failed");
        goto out;
    }
    killer.id = pid;
    if (pthread_create(&tk, NULL, sr_killer, &killer) != 0) {
        test_fail(name, "killer thread failed");
        goto out;
    }

    /* Let the kill reach the victim, then ask about it. */
    sleep_ms(50);
    t0 = sr_now_ms();
    (void) kill(pid, 0);
    probe_ms = sr_now_ms() - t0;

    (void) sr_wait(&killer.done);
    (void) pthread_join(tk, NULL);
    for (int i = 0; i < SLOW_REAP_WAIT_MS && !reaped; i += 2) {
        reaped = waitpid(pid, &status, WNOHANG) == pid;
        if (!reaped)
            sleep_ms(2);
    }
    (void) snprintf(detail, sizeof detail,
                    "kill(victim, 0) took %llu ms during a %d ms hold (limit "
                    "%d), the SIGKILL %llu ms, kill errno %d, victim %s, "
                    "status 0x%x",
                    (unsigned long long) probe_ms, SLOW_REAP_HOLD_MS,
                    SLOW_REAP_PROBE_LIMIT_MS, (unsigned long long) killer.ms,
                    killer.rc, reaped ? "reaped" : "not reaped", status);
    printf("  INFO  %s: %s\n", name, detail);
    check(probe_ms < SLOW_REAP_PROBE_LIMIT_MS && killer.rc == 0 && reaped &&
              WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          name, detail);
    return 0;
out:
    (void) kill(pid, SIGKILL);
    (void) waitpid(pid, &status, 0);
    return 0;
}
