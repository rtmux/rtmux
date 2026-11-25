/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* exec-signal — a signal sent to a process that is exec'ing is not lost.
 *
 * A signal is a notification on the system channel of the image that runs when
 * it is sent, and an exec ends that image: its system thread, its channel and
 * its libc's pending set. Whatever was sent and not delivered has to reach the
 * new image. Three cases, each with a child of this binary:
 *
 * - exec_keeps_pending: the child blocks SIGUSR1, raises it, and execs. The
 *   new image unblocks it and must die of it. Nothing races here: the signal is
 *   in libc's pending set when the exec is asked for.
 * - exec_chain_holds_signal: the child blocks SIGUSR1, says so (a shared
 *   memory object named after it, sized once the signal is blocked), and
 *   execs XSIG_CHAIN_LEN times in a row; the parent sends SIGUSR1 once it has
 *   heard, so somewhere along the chain. The last image unblocks it and must
 *   die of it, so the signal survived every exec after the one it arrived in.
 *   (A signal sent before the block would end the child at once, and pass the
 *   case without any exec.)
 * - exec_loop_takes_sigterm: the child execs itself for ever with SIGTERM at
 *   its default; the parent sends one SIGTERM at a different moment each round.
 *   The child must die of SIGTERM, whichever point of an exec the signal met:
 *   the old image's system thread, the queue of a channel about to be
 *   destroyed, or the gap before the new image has a channel.
 *
 * A child that survives exits with a status of its own, and a child that is
 * not reaped in XSIG_GIVE_UP_MS fails the case; it is then killed until it
 * goes, so that it does not outlive the test.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "test.h"

#define XSIG_CHAIN_LEN 8     /* execs in the chain */
#define XSIG_LOOP_ROUNDS 40  /* children that are sent SIGTERM while exec'ing */
#define XSIG_LOOP_MAX "4000" /* execs after which a looping child gives up */
#define XSIG_GIVE_UP_MS 30000 /* a chain of execs is slow on a loaded host */
#define XSIG_SURVIVED 3 /* the signal did not arrive in the last image */
#define XSIG_RAN_OUT 4  /* a looping child was never signalled */
#define XSIG_NO_EXEC 5  /* exec itself failed */

/* Unblock SIGUSR1 and wait for it: a pending one ends the process inside
 * sigprocmask, one still on its way within the wait.
 */
static int xsig_unblock_and_wait(void)
{
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    (void) sigprocmask(SIG_UNBLOCK, &set, NULL);
    sleep_ms(1500);
    return XSIG_SURVIVED;
}

/* --xsig-chain N: exec N more times, then take the signal. */
int exec_signal_chain(const char *count)
{
    int n = atoi(count);
    char next[16];
    char *argv[] = {"syscall_testing", "--xsig-chain", next, NULL};

    if (n <= 0)
        return xsig_unblock_and_wait();
    snprintf(next, sizeof next, "%d", n - 1);
    (void) execv("/bin/syscall_testing", argv);
    return XSIG_NO_EXEC;
}

/* --xsig-loop N: exec for ever (N times at most), with every signal at its
 * default.
 */
int exec_signal_loop(const char *count)
{
    int n = atoi(count);
    char next[16];
    char *argv[] = {"syscall_testing", "--xsig-loop", next, NULL};

    if (n <= 0)
        return XSIG_RAN_OUT;
    snprintf(next, sizeof next, "%d", n - 1);
    (void) execv("/bin/syscall_testing", argv);
    return XSIG_NO_EXEC;
}

/* The object a child sizes once it has blocked SIGUSR1. */
static void xsig_ready_name(char *buf, size_t len, pid_t pid)
{
    (void) snprintf(buf, len, "/xsig-ready-%d", (int) pid);
}

/* --xsig-child WHAT: set the signal state up, say so, then exec. */
int exec_signal_child(const char *what)
{
    struct flock space;
    char shm[32];
    int fd;
    char *chain[] = {"syscall_testing", "--xsig-chain", NULL, NULL};
    char len[16];
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
        return XSIG_NO_EXEC;
    xsig_ready_name(shm, sizeof shm, getpid());
    memset(&space, 0, sizeof space);
    space.l_whence = SEEK_SET;
    space.l_len = 1;
    if ((fd = shm_open(shm, O_RDWR | O_CREAT, 0600)) < 0 ||
        fcntl(fd, F_ALLOCSP, &space) == -1)
        return XSIG_NO_EXEC;
    (void) close(fd);
    if (!strcmp(what, "pending")) {
        (void) raise(SIGUSR1);
        snprintf(len, sizeof len, "0");
    } else {
        snprintf(len, sizeof len, "%d", XSIG_CHAIN_LEN);
    }
    chain[2] = len;
    (void) execv("/bin/syscall_testing", chain);
    return XSIG_NO_EXEC;
}

/* Wait for pid for at most XSIG_GIVE_UP_MS. Returns 1 with *status set, or 0
 * after killing a child that would not go.
 */
static int xsig_reap(pid_t pid, int *status)
{
    for (int ms = 0; ms < XSIG_GIVE_UP_MS; ms += 2) {
        if (waitpid(pid, status, WNOHANG) == pid)
            return 1;
        sleep_ms(2);
    }
    for (int tries = 0; tries < 2000; tries++) {
        (void) kill(pid, SIGKILL);
        sleep_ms(5);
        if (waitpid(pid, status, WNOHANG) == pid)
            break;
    }
    return 0;
}

/* Spawn a child of this binary with the two arguments. Returns 0 or an errno.
 */
static int xsig_spawn(pid_t *pid, const char *mode, const char *arg)
{
    char *argv[] = {"syscall_testing", (char *) mode, (char *) arg, NULL};

    return posix_spawn(pid, "/bin/syscall_testing", NULL, NULL, argv, NULL);
}

/* Describe how a child ended, for a failure. */
static void xsig_describe(char *buf, size_t len, int reaped, int status)
{
    if (!reaped)
        (void) snprintf(buf, len, "not reaped in %d ms", XSIG_GIVE_UP_MS);
    else if (WIFSIGNALED(status))
        (void) snprintf(buf, len, "killed by signal %d", WTERMSIG(status));
    else
        (void) snprintf(buf, len, "exited %d (%d: the signal was lost)",
                        WEXITSTATUS(status), XSIG_SURVIVED);
}

/* One child that must die of SIGUSR1 after its execs; the parent sends it
 * send_after_ms after the child has blocked it, or leaves it to the child (-1).
 */
static void xsig_usr1_case(const char *name, const char *what, int send_after_ms)
{
    char detail[120], how[80], shm[32];
    int status = 0, reaped, rc, ready = 0;
    pid_t pid;

    if ((rc = xsig_spawn(&pid, "--xsig-child", what)) != 0) {
        (void) snprintf(detail, sizeof detail, "spawn failed, errno %d", rc);
        test_fail(name, detail);
        return;
    }
    xsig_ready_name(shm, sizeof shm, pid);
    for (int ms = 0; ms < XSIG_GIVE_UP_MS && !ready; ms += 2) {
        struct stat st;
        int fd = shm_open(shm, O_RDONLY, 0);

        if (fd >= 0) {
            ready = fstat(fd, &st) == 0 && st.st_size > 0;
            (void) close(fd);
        }
        if (!ready)
            sleep_ms(2);
    }
    (void) shm_unlink(shm);
    if (!ready) {
        (void) xsig_reap(pid, &status);
        test_fail(name, "the child did not get to block the signal");
        return;
    }
    if (send_after_ms >= 0) {
        sleep_ms(send_after_ms);
        if (kill(pid, SIGUSR1) != 0) {
            (void) snprintf(detail, sizeof detail, "kill failed, errno %d",
                            errno);
            (void) xsig_reap(pid, &status);
            test_fail(name, detail);
            return;
        }
    }
    reaped = xsig_reap(pid, &status);
    xsig_describe(how, sizeof how, reaped, status);
    (void) snprintf(detail, sizeof detail, "the last image: %s", how);
    check(reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGUSR1, name,
          detail);
}

static void xsig_loop_case(void)
{
    const char *name = "exec_loop_takes_sigterm";
    char detail[160], how[80] = "";
    int lost = 0, errors = 0, first_errno = 0;

    for (int i = 0; i < XSIG_LOOP_ROUNDS; i++) {
        int status = 0, reaped, rc;
        pid_t pid;

        if ((rc = xsig_spawn(&pid, "--xsig-loop", XSIG_LOOP_MAX)) != 0) {
            errors++;
            if (!first_errno)
                first_errno = rc;
            continue;
        }

        /* A different point of the child's exec cycle each round. */
        sleep_ms(1 + (i * 7) % 23);
        if (kill(pid, SIGTERM) != 0) {
            errors++;
            if (!first_errno)
                first_errno = errno;
        }
        reaped = xsig_reap(pid, &status);
        if (!(reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM)) {
            if (!lost++)
                xsig_describe(how, sizeof how, reaped, status);
        }
    }
    (void) snprintf(detail, sizeof detail,
                    "%d children: %d did not die of SIGTERM (first: %s), %d "
                    "spawn or kill errors (first errno %d)",
                    XSIG_LOOP_ROUNDS, lost, lost ? how : "none", errors,
                    first_errno);
    printf("  INFO  %s: %s\n", name, detail);
    check(lost == 0 && errors == 0, name, detail);
}

void test_exec_signal(void)
{
    xsig_usr1_case("exec_keeps_pending", "pending", -1);
    xsig_usr1_case("exec_chain_holds_signal", "chain", 0);
    xsig_loop_case();
}
