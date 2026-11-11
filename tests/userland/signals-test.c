/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * signals-test.c — exercise the libc signal API.
 *
 * Covers the two-thread signal model:
 * - sigaction round-trip
 * - signal() returns previous handler
 * - raise() runs the handler synchronously before returning
 * - sigprocmask(SIG_BLOCK) holds a raised signal until unblock
 * - kill(getpid(), sig) uses the in-process self-signal path
 * - pthread_sigmask shares that one process-wide mask, returning errno direct
 * - pthread_kill collapses to process-wide delivery (no per-thread target)
 * - cross-process kill(): credential checks, pgrp targets, signal 0, killpg
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>
#include <devctl.h>
#include <devctl/tty.h>
#include <termios.h>

#include "test.h"

/* Handlers can only safely touch volatile sig_atomic_t under POSIX. */
static volatile sig_atomic_t flag_usr1;
static volatile sig_atomic_t flag_usr2;
static volatile sig_atomic_t handler_signo;

static void catch_usr1(int sig)
{
    handler_signo = sig;
    flag_usr1 = 1;
}

static void catch_usr2(int sig)
{
    handler_signo = sig;
    flag_usr2 = 1;
}

/* sigaction round-trip: install a handler, read it back, install a different
 * one, verify the previous one was returned.
 */
static void test_sigaction_roundtrip(void)
{
    struct sigaction act, oact;

    memset(&act, 0, sizeof(act));
    act.sa_handler = catch_usr1;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

    check(sigaction(SIGUSR1, &act, NULL) == 0, "sigaction install SIGUSR1",
          "non-zero return");

    memset(&oact, 0, sizeof(oact));
    check(sigaction(SIGUSR1, NULL, &oact) == 0, "sigaction query SIGUSR1",
          "non-zero return");
    check(oact.sa_handler == catch_usr1, "sigaction round-trip handler matches",
          "handler differs");

    /* Restore SIG_DFL */
    act.sa_handler = SIG_DFL;
    check(sigaction(SIGUSR1, &act, &oact) == 0, "sigaction reset to SIG_DFL",
          "non-zero return");
    check(oact.sa_handler == catch_usr1, "sigaction returns previous handler",
          "previous handler not returned");
}

/* SIGKILL and SIGSTOP must be rejected by sigaction with EINVAL. */
static void test_sigaction_uncatchable(void)
{
    struct sigaction act;
    int rc;

    memset(&act, 0, sizeof(act));
    act.sa_handler = catch_usr1;

    errno = 0;
    rc = sigaction(SIGKILL, &act, NULL);
    check(rc == -1 && errno == EINVAL, "sigaction SIGKILL rejected with EINVAL",
          "wrong rc/errno");

    errno = 0;
    rc = sigaction(SIGSTOP, &act, NULL);
    check(rc == -1 && errno == EINVAL, "sigaction SIGSTOP rejected with EINVAL",
          "wrong rc/errno");
}

/* raise(): the handler runs synchronously, before raise() returns. */
static void test_raise(void)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = catch_usr1;
    sigaction(SIGUSR1, &act, NULL);

    flag_usr1 = 0;
    handler_signo = 0;

    check(raise(SIGUSR1) == 0, "raise(SIGUSR1) returns 0", "non-zero");
    check(flag_usr1 == 1, "raise: handler ran before return", "flag not set");
    check(handler_signo == SIGUSR1, "raise: handler saw correct signo",
          "wrong signo");

    act.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &act, NULL);
}

/* signal() returns the previous handler. */
static void test_signal_returns_prev(void)
{
    void (*prev)(int);

    /* Install catch_usr2 first */
    prev = signal(SIGUSR2, catch_usr2);
    check(prev != SIG_ERR, "signal() install ok", "SIG_ERR");

    /* Replace with catch_usr1; should return catch_usr2 */
    prev = signal(SIGUSR2, catch_usr1);
    check(prev == catch_usr2, "signal() returns previous handler",
          "wrong prev handler");

    /* Reset to SIG_DFL */
    signal(SIGUSR2, SIG_DFL);
}

/* sigprocmask: block SIGUSR1, raise it, verify handler did NOT run. Then
 * unblock and verify the handler runs as we drain pending.
 */
static void test_sigprocmask_block(void)
{
    struct sigaction act;
    sigset_t mask, oldmask;

    memset(&act, 0, sizeof(act));
    act.sa_handler = catch_usr1;
    sigaction(SIGUSR1, &act, NULL);

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    check(sigprocmask(SIG_BLOCK, &mask, &oldmask) == 0,
          "sigprocmask BLOCK SIGUSR1", "non-zero return");

    flag_usr1 = 0;
    raise(SIGUSR1);
    check(flag_usr1 == 0, "raise blocked: handler did NOT run", "flag set");

    /* Unblock — handler should drain immediately */
    check(sigprocmask(SIG_UNBLOCK, &mask, NULL) == 0,
          "sigprocmask UNBLOCK SIGUSR1", "non-zero return");
    check(flag_usr1 == 1, "unblock: pending handler drained", "flag not set");

    /* Restore original mask */
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    act.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &act, NULL);
}

/* kill(getpid(), SIGUSR2): self-signals use the same in-process dispatcher as
 * raise(), avoiding starvation behind a higher-priority caller.
 */
static void test_kill_self(void)
{
    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = catch_usr2;
    sigaction(SIGUSR2, &act, NULL);

    flag_usr2 = 0;
    handler_signo = 0;

    check(kill(getpid(), SIGUSR2) == 0, "kill(getpid, SIGUSR2) ok",
          "non-zero return");

    check(flag_usr2 == 1, "kill: self handler ran before return",
          "flag not set");
    check(handler_signo == SIGUSR2, "kill: handler saw SIGUSR2", "wrong signo");

    act.sa_handler = SIG_DFL;
    sigaction(SIGUSR2, &act, NULL);
}

/* pthread_sigmask operates on RTMux's single process-wide mask — the same state
 * sigprocmask touches — and reports failure by returning the error number
 * directly rather than through errno.
 */
static void test_pthread_sigmask(void)
{
    struct sigaction act;
    sigset_t mask, oldmask, cur;
    int rc;

    memset(&act, 0, sizeof(act));
    act.sa_handler = catch_usr1;
    sigaction(SIGUSR1, &act, NULL);

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    check(pthread_sigmask(SIG_BLOCK, &mask, &oldmask) == 0,
          "pthread_sigmask BLOCK SIGUSR1", "non-zero return");

    flag_usr1 = 0;
    raise(SIGUSR1);
    check(flag_usr1 == 0, "pthread_sigmask block: handler did NOT run",
          "flag set");

    /* The mask is process-global: sigprocmask observes what pthread_sigmask
     * set, proving they share one mask rather than a per-thread copy.
     */
    sigprocmask(SIG_BLOCK, NULL, &cur);
    check(sigismember(&cur, SIGUSR1) == 1,
          "pthread_sigmask shares the process mask with sigprocmask",
          "mask not shared");

    check(pthread_sigmask(SIG_UNBLOCK, &mask, NULL) == 0,
          "pthread_sigmask UNBLOCK SIGUSR1", "non-zero return");
    check(flag_usr1 == 1, "pthread_sigmask unblock: pending drained",
          "flag not set");

    /* pthread convention: the error number is the return value, not -1. */
    rc = pthread_sigmask(-1, &mask, NULL);
    check(rc == EINVAL, "pthread_sigmask bad how returns EINVAL directly",
          "wrong return");

    sigprocmask(SIG_SETMASK, &oldmask, NULL);
    act.sa_handler = SIG_DFL;
    sigaction(SIGUSR1, &act, NULL);
}

/* pthread_kill collapses to process-wide delivery (RTMux has no per-thread
 * signal target), so pthread_kill(self, sig) runs the handler like raise(). sig
 * == 0 is a validity probe that delivers nothing; bad arguments return the
 * error number directly.
 */
static void test_pthread_kill(void)
{
    struct sigaction act;

    memset(&act, 0, sizeof(act));
    act.sa_handler = catch_usr2;
    sigaction(SIGUSR2, &act, NULL);

    flag_usr2 = 0;
    handler_signo = 0;

    check(pthread_kill(pthread_self(), SIGUSR2) == 0,
          "pthread_kill(self, SIGUSR2) returns 0", "non-zero return");
    check(flag_usr2 == 1, "pthread_kill: handler ran (process-wide delivery)",
          "flag not set");
    check(handler_signo == SIGUSR2, "pthread_kill: handler saw SIGUSR2",
          "wrong signo");

    /* sig == 0 probes validity without delivering a signal. */
    flag_usr2 = 0;
    check(pthread_kill(pthread_self(), 0) == 0,
          "pthread_kill(self, 0) validity probe returns 0", "non-zero return");
    check(flag_usr2 == 0, "pthread_kill(self, 0) delivered nothing", "flag set");

    check(pthread_kill(pthread_self(), MAXSIG + 1) == EINVAL,
          "pthread_kill bad signo returns EINVAL", "wrong return");
    check(pthread_kill((pthread_t) 0, SIGUSR2) == ESRCH,
          "pthread_kill non-positive id returns ESRCH", "wrong return");

    act.sa_handler = SIG_DFL;
    sigaction(SIGUSR2, &act, NULL);
}

static void test_signal_set_helpers(void)
{
    sigset_t set;

    errno = 0;
    check(sigfillset(&set) == 0, "sigfillset", "expected success");
    check(sigismember(&set, SIGUSR1) == 1, "sigismember(filled, SIGUSR1)",
          "expected member");
    check(sigdelset(&set, SIGUSR1) == 0, "sigdelset(SIGUSR1)",
          "expected success");
    check(sigismember(&set, SIGUSR1) == 0, "sigismember(deleted, SIGUSR1)",
          "expected non-member");

    errno = 0;
    check(sigdelset(&set, 0) == -1 && errno == EINVAL, "sigdelset(0)",
          "expected EINVAL");
    errno = 0;
    check(sigismember(&set, 0) == -1 && errno == EINVAL, "sigismember(0)",
          "expected EINVAL");
}


/* ---- cross-process kill() ---- */

#define KILL_UID 1000
#define KILL_LOOPS 1500 /* x 10 ms: hard cap so a lost stop cannot hang */

static volatile sig_atomic_t kill_usr1, kill_usr2;

static void kill_catch(int sig)
{
    if (sig == SIGUSR1)
        kill_usr1++;
    else
        kill_usr2++;
}

static sem_t *kill_sem(const char *tag, int idx, int create)
{
    char name[40];

    snprintf(name, sizeof name, "/kc-%s-%d", tag, idx);
    if (create)
        (void) sem_unlink(name);
    return sem_open(name, create ? O_CREAT : 0, 0600, 0);
}

/* Create or post a named semaphore without leaking the descriptor: each
 * sem_open holds an fd, and later tests expect low fd numbers to be free.
 */
static void kill_sem_create(const char *tag, int idx)
{
    sem_t *s = kill_sem(tag, idx, 1);

    if (s != SEM_FAILED)
        (void) sem_close(s);
}

static void kill_post(const char *tag, int idx)
{
    sem_t *s = kill_sem(tag, idx, 0);

    if (s != SEM_FAILED) {
        (void) sem_post(s);
        (void) sem_close(s);
    }
}

static void kill_sem_drop(const char *tag, int idx)
{
    char name[40];

    snprintf(name, sizeof name, "/kc-%s-%d", tag, idx);
    (void) sem_unlink(name);
}

static void nap(void)
{
    struct timespec ts = {.tv_nsec = 10000000L};

    (void) nanosleep(&ts, NULL);
}

static pid_t kill_spawn(int idx, int uid, int pgid);

/* Victim: --kill-child idx uid pgid. pgid < 0 keeps the inherited group, 0
 * starts a new one, > 0 joins it. Group leader (pgid -2) calls setsid() and sends
 * kill(0, SIGUSR2) when "go" is posted.
 */
static int kill_victim(int idx, uid_t uid, pid_t pgid)
{
    struct sigaction sa;
    sem_t *ready = kill_sem("ready", idx, 0), *ack = kill_sem("ack", idx, 0);
    sem_t *stop = kill_sem("stop", idx, 0), *go = kill_sem("go", idx, 0);
    int seen1 = 0, seen2 = 0;

    if (ready == SEM_FAILED || ack == SEM_FAILED || stop == SEM_FAILED ||
        go == SEM_FAILED)
        return 100;
    pid_t members[2] = {-1, -1};

    if (pgid == -2) {
        /* Group leader: own session and group, then two members that inherit
         * both from it. */
        if (setsid() < 0)
            return 104;
        for (int m = 0; m < 2; m++)
            if ((members[m] = kill_spawn(3 + m, 0, -1)) < 0)
                return 105;
    } else if (pgid > 0 && setpgid(0, pgid) != 0) {
        return 101;
    }
    if (uid != 0 && (setgid(uid) != 0 || setuid(uid) != 0))
        return 102;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = kill_catch;
    if (sigaction(SIGUSR1, &sa, NULL) != 0 || sigaction(SIGUSR2, &sa, NULL) != 0)
        return 103;
    (void) sem_post(ready);

    for (int i = 0; i < KILL_LOOPS; i++) {
        while (seen1 < kill_usr1 || seen2 < kill_usr2) {
            seen1 = kill_usr1 > seen1 ? seen1 + 1 : seen1;
            seen2 = kill_usr2 > seen2 ? seen2 + 1 : seen2;
            (void) sem_post(ack);
        }
        if (sem_trywait(go) == 0)
            (void) kill(0, SIGUSR2);
        if (sem_trywait(stop) == 0)
            break;
        nap();
    }
    if (pgid == -2) {
        /* The members must each have run two SIGUSR1 and one SIGUSR2 too. */
        for (int m = 0; m < 2; m++) {
            int st = 0;

            kill_post("stop", 3 + m);
            if (waitpid(members[m], &st, 0) != members[m] || !WIFEXITED(st) ||
                WEXITSTATUS(st) != 21)
                return 106;
        }
    }
    return 10 * kill_usr1 + kill_usr2;
}

/* Sender: --kill-sender root_pid victim_pid. Runs as uid 1000. */
static int kill_sender(pid_t root_pid, pid_t same_uid_pid)
{
    if (setgid(KILL_UID) != 0 || setuid(KILL_UID) != 0)
        return 1;
    errno = 0;
    if (kill(root_pid, SIGUSR1) != -1 || errno != EPERM)
        return 2;
    errno = 0;
    if (kill(root_pid, 0) != -1 || errno != EPERM)
        return 3;
    if (kill(same_uid_pid, 0) != 0)
        return 4;
    errno = 0;
    if (kill(same_uid_pid, NSIG) != -1 || errno != EINVAL)
        return 5;
    errno = 0;
    if (kill(getpid(), -1) != -1 || errno != EINVAL)
        return 6;
    errno = 0;
    if (kill(-30000, 0) != -1 || errno != ESRCH)
        return 7;
    errno = 0;
    if (killpg(-1, SIGUSR1) != -1 || errno != EINVAL)
        return 8;
    /* A process with no group of its own signals only itself with kill(0). */
    if (kill(0, 0) != 0)
        return 10;
    /* Broadcast: reaches the same-uid sibling, never root or a server. */
    if (kill(-1, SIGUSR1) != 0)
        return 9;
    return 0;
}

static int si_sender(pid_t target);
static int si_prio_child(void);

static int eintr_remote_child(void);
static int jc_child(int argc, char **argv);

int kill_child(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "--kill-eintr"))
        return eintr_remote_child();
    if (argc >= 3 && !strcmp(argv[1], "--kill-jc"))
        return jc_child(argc, argv);
    if (argc == 5 && !strcmp(argv[1], "--kill-child"))
        return kill_victim(atoi(argv[2]), (uid_t) atoi(argv[3]), atoi(argv[4]));
    if (argc == 3 && !strcmp(argv[1], "--kill-sigsender"))
        return si_sender(atoi(argv[2]));
    if (argc == 3 && !strcmp(argv[1], "--kill-sigprio"))
        return si_prio_child();
    if (argc == 4 && !strcmp(argv[1], "--kill-sender"))
        return kill_sender(atoi(argv[2]), atoi(argv[3]));
    return -1;
}

static pid_t kill_spawn(int idx, int uid, int pgid)
{
    char a[16], b[16], c[16];
    char *argv[] = {"syscall_testing", "--kill-child", a, b, c, NULL};
    pid_t pid;

    snprintf(a, sizeof a, "%d", idx);
    snprintf(b, sizeof b, "%d", uid);
    snprintf(c, sizeof c, "%d", pgid);
    kill_sem_create("ready", idx);
    kill_sem_create("ack", idx);
    kill_sem_create("stop", idx);
    kill_sem_create("go", idx);
    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0)
        return -1;
    return pid;
}

static int kill_wait_sem(const char *tag, int idx, int ms)
{
    struct timespec dl;
    sem_t *s = kill_sem(tag, idx, 0);

    if (s == SEM_FAILED)
        return 0;
    (void) clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += ms / 1000;
    dl.tv_nsec += (long) (ms % 1000) * 1000000L;
    if (dl.tv_nsec >= 1000000000L) {
        dl.tv_sec++;
        dl.tv_nsec -= 1000000000L;
    }
    int ok = sem_timedwait(s, &dl) == 0;

    (void) sem_close(s);
    return ok;
}

/* Stop a victim and return its exit code (10 * usr1 + usr2), or -1. */
static int kill_reap(int idx, pid_t pid)
{
    int status = 0;

    if (pid < 0)
        return -1;
    kill_post("stop", idx);
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status))
        return -1;
    return WEXITSTATUS(status);
}

static void kill_cleanup(int first, int last)
{
    static const char *const tags[] = {"ready", "ack", "stop", "go"};

    for (int i = first; i <= last; i++)
        for (size_t t = 0; t < 4; t++)
            kill_sem_drop(tags[t], i);
}

/* ---- job control: stop/continue, controlling terminal, background access ---- */

/* A process stops and continues itself (its system thread holds the other
 * threads) and reports each change to roottask, which sets
 * RTMUX_FLG_PROC_STOPPED/CONTINUED and the siginfo waitpid(WUNTRACED |
 * WCONTINUED) reads. The terminal servers police a background process on its
 * controlling terminal with EIO or SIGTTIN/SIGTTOU + EINTR.
 *
 * Children: --kill-jc MODE ARG. "spin" posts a tick every few ms so the parent
 * can tell whether it runs; the others are the terminal fixtures. A child
 * announces itself and its progress on named semaphores (index JC_IDX).
 */

#define JC_IDX 9
#define JC_SPIN_LOOPS 1000 /* x 5 ms: hard cap so a lost stop cannot hang */
#define JC_WAIT_MS 5000

static volatile sig_atomic_t jc_cont, jc_tstp;

static void jc_catch_cont(int sig)
{
    (void) sig;
    jc_cont = 1;
}

static void jc_catch_tstp(int sig)
{
    (void) sig;
    jc_tstp++;
}

/* Wait for pid to report a state change, polling so a lost one cannot hang the
 * suite. Returns the waitpid() result, 0 on timeout.
 */
static pid_t jc_wait(pid_t pid, int *st, int opts, int ms)
{
    pid_t r = 0;

    for (int t = 0; t < ms / 10; t++) {
        r = waitpid(pid, st, opts | WNOHANG);
        if (r != 0)
            return r;
        nap();
    }
    return 0;
}

static int jc_ticks(sem_t *tick)
{
    int n = 0;

    while (sem_trywait(tick) == 0)
        n++;
    return n;
}

/* Child: tick until told to stop. argv mode "spin" keeps default dispositions
 * except for a SIGCONT handler; "spinhard" ignores SIGTSTP, blocks and ignores
 * SIGCONT and blocks everything else too, to show that SIGSTOP and SIGCONT
 * still act; "spinhandler" catches SIGTSTP.
 */
static int jc_spin(const char *mode)
{
    struct sigaction sa;
    sigset_t all;
    sem_t *ready = kill_sem("jcready", JC_IDX, 0);
    sem_t *tick = kill_sem("jctick", JC_IDX, 0);
    sem_t *stop = kill_sem("jcstop", JC_IDX, 0);

    if (ready == SEM_FAILED || tick == SEM_FAILED || stop == SEM_FAILED)
        return 100;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = jc_catch_cont;
    if (!strcmp(mode, "spinhard"))
        sa.sa_handler = SIG_IGN;
    if (sigaction(SIGCONT, &sa, NULL) != 0)
        return 101;
    if (!strcmp(mode, "spinhard")) {
        sa.sa_handler = SIG_IGN;
        if (sigaction(SIGTSTP, &sa, NULL) != 0)
            return 102;
        sigfillset(&all);
        if (sigprocmask(SIG_SETMASK, &all, NULL) != 0)
            return 103;
    } else if (!strcmp(mode, "spinhandler")) {
        sa.sa_handler = jc_catch_tstp;
        if (sigaction(SIGTSTP, &sa, NULL) != 0)
            return 102;
    }
    (void) sem_post(ready);
    for (int i = 0; i < JC_SPIN_LOOPS; i++) {
        (void) sem_post(tick);
        if (sem_trywait(stop) == 0)
            break;
        nap();
    }
    return 10 * (int) jc_cont + (int) jc_tstp;
}

static int jc_tty_open(void)
{
    return open("/dev/tty", O_RDWR | O_NONBLOCK | O_NOCTTY);
}

/* Wait here until SIGCONT's handler ran, i.e. until the parent continued us. */
static int jc_await_cont(void)
{
    for (int n = 0; n < JC_WAIT_MS / 10 && !jc_cont; n++)
        nap();
    return jc_cont ? 0 : 1;
}

/* Hold the child until the parent has seen its continue reported: waitpid()
 * reports an exit before a continue, so a child that left at once would hide it.
 */
static void jc_linger(void)
{
    sem_t *stop = kill_sem("jcstop", JC_IDX, 0);
    struct timespec dl;

    if (stop == SEM_FAILED)
        return;
    (void) clock_gettime(CLOCK_REALTIME, &dl);
    dl.tv_sec += JC_WAIT_MS / 1000;
    (void) sem_timedwait(stop, &dl);
    (void) sem_close(stop);
}

/* Child on the terminal, in a group of its own (so in the background). */
static int jc_tty_child(const char *mode)
{
    struct sigaction sa;
    sigset_t set;
    sem_t *intr = kill_sem("jcintr", JC_IDX, 0);
    char c;
    int fd = jc_tty_open();
    int err;
    ssize_t r;

    if (fd < 0 || intr == SEM_FAILED)
        return 100;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = jc_catch_cont;
    if (sigaction(SIGCONT, &sa, NULL) != 0)
        return 101;

    if (!strcmp(mode, "read")) {
        /* Background read: SIGTTIN and EINTR, and once the group is foreground
         * and we are continued the read goes through (no input: EAGAIN).
         */
        /* The stop takes hold asynchronously, possibly before the next line, so
         * announce first and look at the result once continued.
         */
        (void) sem_post(intr);
        r = read(fd, &c, 1);
        err = errno;
        if (jc_await_cont())
            return 11;
        if (r != -1 || err != EINTR)
            return 10;
        r = read(fd, &c, 1);
        err = r == -1 && errno == EAGAIN ? 0 : 12;
        jc_linger();
        return err;
    }
    if (!strcmp(mode, "readblock")) {
        /* SIGTTIN ignored, then blocked: EIO and no signal either way. */
        sa.sa_handler = SIG_IGN;
        if (sigaction(SIGTTIN, &sa, NULL) != 0)
            return 20;
        r = read(fd, &c, 1);
        if (r != -1 || errno != EIO)
            return 21;
        sa.sa_handler = SIG_DFL;
        if (sigaction(SIGTTIN, &sa, NULL) != 0)
            return 22;
        sigemptyset(&set);
        sigaddset(&set, SIGTTIN);
        if (sigprocmask(SIG_BLOCK, &set, NULL) != 0)
            return 23;
        r = read(fd, &c, 1);
        return r == -1 && errno == EIO ? 0 : 24;
    }
    if (!strcmp(mode, "write")) {
        /* Zero-length writes reach the gate without printing anything. Without
         * TOSTOP a background write is allowed; the parent sets it for the
         * rest: SIGTTOU and EINTR, then through once SIGTTOU is ignored.
         */
        struct termios t;

        if (tcgetattr(fd, &t) != 0)
            return 30;
        if (!(t.c_lflag & TOSTOP))
            return write(fd, "", 0) == 0 ? 0 : 31;
        (void) sem_post(intr);
        r = write(fd, "", 0);
        err = errno;
        if (jc_await_cont())
            return 33;
        if (r != -1 || err != EINTR)
            return 32;
        sa.sa_handler = SIG_IGN;
        if (sigaction(SIGTTOU, &sa, NULL) != 0)
            return 34;
        err = write(fd, "", 0) == 0 ? 0 : 35;
        jc_linger();
        return err;
    }
    if (!strcmp(mode, "orphan")) {
        /* Our parent has exited, so this group has no link to the rest of the
         * session: reads fail with EIO instead of stopping us.
         */
        sem_t *go = kill_sem("jcgo", JC_IDX, 0);
        struct timespec dl;

        if (go == SEM_FAILED)
            return 40;
        (void) clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += JC_WAIT_MS / 1000;
        if (sem_timedwait(go, &dl) != 0)
            return 41;
        r = read(fd, &c, 1);
        if (r != -1 || errno != EIO)
            return 42;
        (void) sem_post(intr);
        return 0;
    }
    return 99;
}

static pid_t jc_spawn(const char *mode, int newgrp)
{
    char *argv[] = {"syscall_testing", "--kill-jc", (char *) mode, NULL};
    posix_spawnattr_t attr;
    pid_t pid;
    int rc;

    posix_spawnattr_init(&attr);
    if (newgrp) {
        posix_spawnattr_setpgroup(&attr, 0);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    }
    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, &attr, argv, NULL);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
}

/* Middle process of the orphan fixture: start the group leader in a group of its
 * own, then exit, leaving that group orphaned.
 */
static int jc_orphan_parent(void)
{
    sem_t *spawned = kill_sem("jcspawned", JC_IDX, 0);

    if (spawned == SEM_FAILED || jc_spawn("orphan", 1) < 0)
        return 1;
    (void) sem_post(spawned);
    return 0;
}

/* A new session has no controlling terminal; it cannot take another session's
 * terminal, and a raw tcsetpgrp on a terminal that is not its own fails.
 * argv[3] names a terminal known to belong to the suite's own session.
 */
static int jc_session_child(const char *owned)
{
    pid_t pg = getpid();
    int fd;

    if (setsid() < 0)
        return 1;
    errno = 0;
    if (open("/dev/tty", O_RDWR | O_NOCTTY) != -1 || errno != ENXIO)
        return 2;
    if (!owned || !*owned)
        return 0;
    if ((fd = open(owned, O_RDWR | O_NOCTTY)) < 0)
        return 3;
    if (devctl(fd, DEVCTL_TTY_SET_PGRP, &pg, sizeof pg, NULL) != ENOTTY)
        return 4;
    errno = 0;
    if (tcsetsid(fd, pg) != -1 || errno != EPERM)
        return 5;
    return 0;
}

static int jc_child(int argc, char **argv)
{
    const char *mode = argv[2];

    if (!strncmp(mode, "spin", 4))
        return jc_spin(mode);
    if (!strcmp(mode, "orphan-parent"))
        return jc_orphan_parent();
    if (!strcmp(mode, "session"))
        return jc_session_child(argc > 3 ? argv[3] : NULL);
    return jc_tty_child(mode);
}

static void jc_sems_create(void)
{
    static const char *const tags[] = {"jcready", "jctick", "jcstop", "jcintr",
                                       "jcgo", "jcspawned"};

    for (size_t t = 0; t < sizeof tags / sizeof tags[0]; t++)
        kill_sem_create(tags[t], JC_IDX);
}

static void jc_sems_drop(void)
{
    static const char *const tags[] = {"jcready", "jctick", "jcstop", "jcintr",
                                       "jcgo", "jcspawned"};

    for (size_t t = 0; t < sizeof tags / sizeof tags[0]; t++)
        kill_sem_drop(tags[t], JC_IDX);
}

/* Stop and continue a process that has no terminal: waitpid reports both once,
 * the stopped process runs nothing, and SIGSTOP/SIGCONT act whatever the mask
 * and dispositions say.
 */
static void test_jobctl_stop(const char *mode, const char *name, int signo)
{
    int st = 0, ticks;
    pid_t pid;
    sem_t *tick;

    jc_sems_create();
    pid = jc_spawn(mode, 0);
    if (pid < 0 || !kill_wait_sem("jcready", JC_IDX, JC_WAIT_MS)) {
        test_fail(name, "child did not start");
        jc_sems_drop();
        return;
    }
    tick = kill_sem("jctick", JC_IDX, 0);
    if (tick == SEM_FAILED) {
        test_fail(name, "tick semaphore");
        jc_sems_drop();
        return;
    }
    nap();
    nap();
    check(jc_ticks(tick) > 0, "jobctl child runs", "the child must tick");

    check(kill(pid, signo) == 0, "jobctl stop signal", "kill must succeed");
    if (!strcmp(mode, "spinhard")) {
        /* Blocked and ignored SIGTSTP does nothing; SIGSTOP below still stops. */
        check(jc_wait(pid, &st, WUNTRACED, 300) == 0,
              "jobctl ignored SIGTSTP does not stop",
              "an ignored stop signal must not stop the process");
        check(kill(pid, SIGSTOP) == 0, "jobctl SIGSTOP", "kill must succeed");
        signo = SIGSTOP;
    }
    check(jc_wait(pid, &st, WUNTRACED, JC_WAIT_MS) == pid && WIFSTOPPED(st) &&
              WSTOPSIG(st) == signo,
          name, "waitpid(WUNTRACED) must report the stop signal");
    (void) jc_ticks(tick);
    for (int i = 0; i < 10; i++)
        nap();
    ticks = jc_ticks(tick);
    check(ticks == 0, "jobctl stopped child runs nothing",
          "a stopped process must not execute");
    check(jc_wait(pid, &st, WUNTRACED, 50) == 0,
          "jobctl stop reported once",
          "a second waitpid(WUNTRACED) must not see the same stop");

    check(kill(pid, SIGCONT) == 0, "jobctl SIGCONT", "kill must succeed");
    check(jc_wait(pid, &st, WCONTINUED, JC_WAIT_MS) == pid && WIFCONTINUED(st),
          "jobctl continue reported",
          "waitpid(WCONTINUED) must report the continue");
    for (int i = 0; i < 20 && !(ticks = jc_ticks(tick)); i++)
        nap();
    check(ticks > 0, "jobctl continued child runs again",
          "a continued process must execute");

    if (!strcmp(mode, "spin")) {
        /* A stopped process still dies to SIGKILL. */
        check(kill(pid, SIGSTOP) == 0 &&
                  jc_wait(pid, &st, WUNTRACED, JC_WAIT_MS) == pid &&
                  WIFSTOPPED(st),
              "jobctl stop again", "a continued process stops again");
        (void) kill(pid, SIGKILL);
        check(jc_wait(pid, &st, 0, JC_WAIT_MS) == pid && !WIFSTOPPED(st),
              "jobctl SIGKILL ends a stopped process",
              "the stopped process must be reaped, not stay stopped");
    } else {
        kill_post("jcstop", JC_IDX);
        check(jc_wait(pid, &st, 0, JC_WAIT_MS) == pid && WIFEXITED(st),
              "jobctl child exits", "the child must exit once told to");
    }
    (void) sem_close(tick);
    jc_sems_drop();
}

/* SIGTSTP with a handler runs it instead of stopping. */
static void test_jobctl_handler(void)
{
    int st = 0;
    pid_t pid;

    jc_sems_create();
    pid = jc_spawn("spinhandler", 0);
    if (pid < 0 || !kill_wait_sem("jcready", JC_IDX, JC_WAIT_MS)) {
        test_fail("jobctl handler fixture", "child did not start");
        jc_sems_drop();
        return;
    }
    check(kill(pid, SIGTSTP) == 0, "jobctl handled SIGTSTP", "kill must succeed");
    check(jc_wait(pid, &st, WUNTRACED, 300) == 0,
          "jobctl caught SIGTSTP does not stop",
          "a caught stop signal must run its handler, not stop");
    check(kill(pid, SIGSTOP) == 0 &&
              jc_wait(pid, &st, WUNTRACED, JC_WAIT_MS) == pid && WIFSTOPPED(st),
          "jobctl SIGSTOP with a handler installed", "SIGSTOP always stops");
    (void) kill(pid, SIGCONT);
    check(jc_wait(pid, &st, WCONTINUED, JC_WAIT_MS) == pid && WIFCONTINUED(st),
          "jobctl SIGCONT with a handler installed", "SIGCONT always continues");
    kill_post("jcstop", JC_IDX);
    check(jc_wait(pid, &st, 0, JC_WAIT_MS) == pid && WIFEXITED(st) &&
              WEXITSTATUS(st) == 11,
          "jobctl handlers ran",
          "SIGTSTP handler and SIGCONT handler each ran once");
    jc_sems_drop();
}

/* Make child the foreground group, continue it, and expect it to finish with
 * status 0. The suite is then in the background; it ignores SIGTTOU (the POSIX
 * way for a background tcsetpgrp to succeed) to take the terminal back.
 */
static int jc_finish_in_foreground(int fd, pid_t pid, const char *name)
{
    int st = 0, ok;

    ok = tcsetpgrp(fd, pid) == 0 && tcgetpgrp(fd) == pid;
    check(ok, name, "the foreground group must be settable to the child's");
    (void) kill(pid, SIGCONT);
    check(jc_wait(pid, &st, WCONTINUED, JC_WAIT_MS) == pid && WIFCONTINUED(st),
          "jobctl terminal child continued", "waitpid(WCONTINUED)");
    kill_post("jcstop", JC_IDX);
    return jc_wait(pid, &st, 0, JC_WAIT_MS) == pid && WIFEXITED(st)
               ? WEXITSTATUS(st)
               : -1;
}

static void jc_take_back(int fd)
{
    struct sigaction ign, old;

    memset(&ign, 0, sizeof ign);
    ign.sa_handler = SIG_IGN;
    (void) sigaction(SIGTTOU, &ign, &old);
    check(tcsetpgrp(fd, getpgrp()) == 0 && tcgetpgrp(fd) == getpgrp(),
          "jobctl background tcsetpgrp with SIGTTOU ignored",
          "a background caller that ignores SIGTTOU may set the group");
    (void) sigaction(SIGTTOU, &old, NULL);
}

/* The controlling-terminal half. Runs only when the suite is the foreground
 * group of its controlling terminal, as when started from init or a shell.
 */
static void test_jobctl_tty(void)
{
    static const char *const names[] = {"/dev/text", "/dev/ser1"};
    char owned[16] = "";
    struct termios t, saved;
    int st = 0, fd = open("/dev/tty", O_RDWR | O_NOCTTY);
    pid_t pid, mid;
    int termios_saved;

    if (fd < 0 || !isatty(fd) || tcgetpgrp(fd) != getpgrp()) {
        printf("  SKIP: jobctl tty fixtures (fd %d, isatty %d, tcgetpgrp %d, "
               "getpgrp %d, errno %d)\n",
               fd, fd >= 0 ? isatty(fd) : -1, fd >= 0 ? tcgetpgrp(fd) : -1,
               (int) getpgrp(), errno);
        if (fd >= 0)
            close(fd);
        return;
    }

    /* tcsetpgrp: the group must exist in the caller's session (checked by the
     * libc call and, for a raw devctl, by the terminal server itself).
     */
    check(tcsetpgrp(fd, getpgrp()) == 0 && tcgetpgrp(fd) == getpgrp(),
          "jobctl tcsetpgrp round trip", "the foreground group must round trip");
    errno = 0;
    check(tcsetpgrp(fd, 1000000) == -1 && errno == EPERM,
          "jobctl tcsetpgrp unknown group", "EPERM for a group that is absent");
    {
        pid_t bogus[] = {1000000, 1};

        for (size_t i = 0; i < sizeof bogus / sizeof bogus[0]; i++) {
            check(devctl(fd, DEVCTL_TTY_SET_PGRP, &bogus[i], sizeof bogus[i],
                         NULL) == EPERM,
                  "jobctl raw SET_PGRP outside the session",
                  "the terminal server must refuse a foreign group itself");
        }
    }
    check(tcgetpgrp(fd) == getpgrp(), "jobctl refused SET_PGRP kept the group",
          "a refused set must not change the foreground group");

    /* A terminal known to be ours, for the session fixture. */
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        int f = open(names[i], O_RDWR | O_NOCTTY);

        if (f >= 0 && tcgetpgrp(f) == getpgrp())
            snprintf(owned, sizeof owned, "%s", names[i]);
        if (f >= 0)
            close(f);
    }
    {
        char *argv[] = {"syscall_testing", "--kill-jc", "session", owned, NULL};
        pid_t s;

        check(posix_spawn(&s, "/bin/syscall_testing", NULL, NULL, argv, NULL) ==
                      0 &&
                  waitpid(s, &st, 0) == s && WIFEXITED(st) &&
                  WEXITSTATUS(st) == 0,
              "jobctl new session (no /dev/tty, no stolen terminal)",
              "a new session must see ENXIO from /dev/tty, ENOTTY from a raw "
              "tcsetpgrp, and EPERM taking a terminal that is in use");
    }

    jc_sems_create();

    /* Background read: SIGTTIN and EINTR; foreground and continued it works. */
    pid = jc_spawn("read", 1);
    if (pid < 0 || !kill_wait_sem("jcintr", JC_IDX, JC_WAIT_MS)) {
        test_fail("jobctl background read", "child did not report EINTR");
    } else {
        check(jc_wait(pid, &st, WUNTRACED, JC_WAIT_MS) == pid &&
                  WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTIN,
              "jobctl background read stops with SIGTTIN",
              "a background read must fail EINTR and stop the group");
        check(jc_finish_in_foreground(fd, pid, "jobctl tcsetpgrp to a child") ==
                  0,
              "jobctl read as foreground after SIGCONT",
              "the continued foreground child must read without error");
        jc_take_back(fd);
    }

    /* SIGTTIN blocked or ignored: EIO, and no stop. */
    pid = jc_spawn("readblock", 1);
    check(pid > 0 && jc_wait(pid, &st, WUNTRACED, JC_WAIT_MS) == pid &&
              WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "jobctl background read EIO when SIGTTIN is held",
          "an ignored or blocked SIGTTIN must give EIO");

    /* Orphaned group: EIO. The middle process exits, orphaning its child. */
    jc_sems_drop();
    jc_sems_create();
    mid = jc_spawn("orphan-parent", 0);
    if (mid < 0 || !kill_wait_sem("jcspawned", JC_IDX, JC_WAIT_MS) ||
        jc_wait(mid, &st, 0, JC_WAIT_MS) != mid) {
        test_fail("jobctl orphaned group", "fixture did not start");
    } else {
        /* The orphan is not our child any more: it reports EIO on jcintr. */
        kill_post("jcgo", JC_IDX);
        check(kill_wait_sem("jcintr", JC_IDX, JC_WAIT_MS),
              "jobctl orphaned group read EIO",
              "a background read in an orphaned group must fail EIO");
    }

    /* TOSTOP: a background write stops with SIGTTOU, or goes through. */
    termios_saved = tcgetattr(fd, &saved) == 0;
    if (termios_saved) {
        pid = jc_spawn("write", 1);
        check(pid > 0 && jc_wait(pid, &st, WUNTRACED, JC_WAIT_MS) == pid &&
                  WIFEXITED(st) && WEXITSTATUS(st) == 0,
              "jobctl background write without TOSTOP",
              "a background write is allowed unless TOSTOP is set");

        t = saved;
        t.c_lflag |= TOSTOP;
        check(tcsetattr(fd, TCSANOW, &t) == 0, "jobctl set TOSTOP", "tcsetattr");
        jc_sems_drop();
        jc_sems_create();
        pid = jc_spawn("write", 1);
        if (pid < 0 || !kill_wait_sem("jcintr", JC_IDX, JC_WAIT_MS)) {
            test_fail("jobctl background write under TOSTOP", "no EINTR");
        } else {
            check(jc_wait(pid, &st, WUNTRACED, JC_WAIT_MS) == pid &&
                      WIFSTOPPED(st) && WSTOPSIG(st) == SIGTTOU,
                  "jobctl background write stops with SIGTTOU",
                  "TOSTOP: a background write must fail EINTR and stop");
            (void) kill(pid, SIGCONT);
            check(jc_wait(pid, &st, WCONTINUED, JC_WAIT_MS) == pid &&
                      WIFCONTINUED(st),
                  "jobctl background write child continued",
                  "waitpid(WCONTINUED)");
            kill_post("jcstop", JC_IDX);
            check(jc_wait(pid, &st, 0, JC_WAIT_MS) == pid && WIFEXITED(st) &&
                      WEXITSTATUS(st) == 0,
                  "jobctl background write with SIGTTOU ignored",
                  "ignoring SIGTTOU lets a background write through");
        }
        (void) tcsetattr(fd, TCSANOW, &saved);
    }
    jc_sems_drop();
    check(tcgetpgrp(fd) == getpgrp(), "jobctl foreground group restored",
          "the suite must be left as the foreground group");
    close(fd);
}

/* Authority: _PROC_JOBCTL's group operations are for the terminal server of a
 * session's controlling terminal. An ordinary process must not be able to stop
 * or signal a group, nor change a foreground group, through them.
 */
static int jc_raw(unsigned subtype, pid_t pid, int arg, int arg2,
                  struct _proc_jobctl_reply *rep)
{
    struct _proc_jobctl msg;
    struct _proc_jobctl_reply r;

    memset(&msg, 0, sizeof msg);
    msg.type = _PROC_JOBCTL;
    msg.subtype = (uint16_t) subtype;
    msg.pid = pid;
    msg.arg = arg;
    msg.arg2 = arg2;
    errno = 0;
    if (msg_sendnc(ROOTTASK_COID, &msg, sizeof msg, &r, sizeof r) == -1)
        return errno;
    if (rep)
        *rep = r;
    return 0;
}

static void test_jobctl_authority(void)
{
    struct _proc_jobctl_reply rep;
    int st = 0, ticks;
    pid_t pid;
    sem_t *tick;

    jc_sems_create();
    pid = jc_spawn("spin", 0);
    tick = kill_sem("jctick", JC_IDX, 0);
    if (pid < 0 || !kill_wait_sem("jcready", JC_IDX, JC_WAIT_MS) ||
        tick == SEM_FAILED) {
        test_fail("jobctl authority", "child did not start");
        if (pid > 0) {
            (void) kill(pid, SIGKILL);
            (void) jc_wait(pid, &st, 0, JC_WAIT_MS);
        }
        jc_sems_drop();
        return;
    }
    check(jc_raw(_PROC_JOBCTL_SIGBG, pid, SIGTTIN, 0, NULL) == EPERM,
          "jobctl SIGBG refused from a non-server",
          "only the terminal server of the session may signal a group");
    check(jc_raw(_PROC_JOBCTL_SIGFG, getpgrp(), SIGINT, 0, NULL) == ESRCH,
          "jobctl SIGFG refused from a non-server",
          "a process that does not serve the terminal must signal nobody");
    check(jc_raw(_PROC_JOBCTL_SIGFG, getpgrp(), SIGKILL, 0, NULL) == EINVAL,
          "jobctl SIGFG only the termios signals",
          "SIGFG must refuse a signal no terminal character raises");
    check(jc_raw(_PROC_JOBCTL_SETPGRP, pid, getpgrp(), 0, NULL) == ENOTTY,
          "jobctl SETPGRP refused from a non-server",
          "only the terminal server may change a foreground group");
    check(jc_raw(_PROC_JOBCTL_QUERY, pid, 0, 0, &rep) == 0 &&
              !(rep.flags & _JOBCTL_CTTY),
          "jobctl QUERY claims no terminal for a non-server",
          "a client's standing is reported only to its terminal's server");
    check(jc_raw(_PROC_JOBCTL_STATE, 0, 99, 0, NULL) == EINVAL,
          "jobctl STATE rejects an unknown event", "EINVAL");
    check(jc_raw(99, 0, 0, 0, NULL) == ENOSYS,
          "jobctl rejects an unknown subtype", "ENOSYS");

    /* None of that may have stopped the child. */
    (void) jc_ticks(tick);
    for (int i = 0; i < 5; i++)
        nap();
    ticks = jc_ticks(tick);
    check(ticks > 0 && jc_wait(pid, &st, WUNTRACED, 50) == 0,
          "jobctl refused requests left the child running",
          "a refused group operation must not stop or signal anyone");
    kill_post("jcstop", JC_IDX);
    (void) jc_wait(pid, &st, 0, JC_WAIT_MS);
    (void) sem_close(tick);
    jc_sems_drop();
}

static void test_jobctl(void)
{
    test_jobctl_stop("spin", "jobctl SIGTSTP stops, waitpid(WUNTRACED)", SIGTSTP);
    test_jobctl_stop("spinhard", "jobctl SIGSTOP stops a blocked process",
                     SIGTSTP);
    test_jobctl_handler();
    test_jobctl_authority();
    test_jobctl_tty();
}

static void test_kill_cross_uid(void)
{
    char a[16], b[16];
    char *argv[] = {"syscall_testing", "--kill-sender", a, b, NULL};
    pid_t root_v, user_v, sender;
    int status = -1;

    root_v = kill_spawn(0, 0, -1);
    user_v = kill_spawn(1, KILL_UID, -1);
    if (root_v < 0 || user_v < 0 || !kill_wait_sem("ready", 0, 5000) ||
        !kill_wait_sem("ready", 1, 5000)) {
        test_fail("kill_cross_uid fixture", "victims did not start");
        return;
    }
    snprintf(a, sizeof a, "%d", (int) root_v);
    snprintf(b, sizeof b, "%d", (int) user_v);
    if (posix_spawn(&sender, "/bin/syscall_testing", NULL, NULL, argv, NULL)) {
        test_fail("kill_cross_uid spawn", strerror(errno));
        return;
    }
    check(waitpid(sender, &status, 0) == sender && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "kill_cross_uid sender checks (EPERM, sig0, EINVAL, ESRCH, broadcast)",
          "an unprivileged sender must be refused cross-uid kill and signal 0");
    check(kill_wait_sem("ack", 1, 3000), "kill_broadcast_unpriv reaches same uid",
          "broadcast must reach the uid-1000 victim");
    check(kill_reap(0, root_v) == 0, "kill_broadcast_unpriv spares root",
          "the root victim must not run a handler");
    check(kill_reap(1, user_v) == 10, "kill_broadcast_unpriv same-uid once",
          "the uid-1000 victim must run SIGUSR1 exactly once");
    kill_cleanup(0, 1);
}

static void test_kill_pgrp(void)
{
    pid_t leader;

    /* The parent creates every semaphore; the leader spawns members 3 and 4. */
    for (int i = 3; i <= 4; i++) {
        kill_sem_create("ready", i);
        kill_sem_create("ack", i);
        kill_sem_create("stop", i);
        kill_sem_create("go", i);
    }
    leader = kill_spawn(2, 0, -2);
    if (leader < 0 || !kill_wait_sem("ready", 2, 5000) ||
        !kill_wait_sem("ready", 3, 5000) || !kill_wait_sem("ready", 4, 5000)) {
        char why[48];

        snprintf(why, sizeof why, "group did not start (leader rc %d)",
                 kill_reap(2, leader));
        test_fail("kill_pgrp fixture", why);
        kill_cleanup(2, 4);
        return;
    }
    check(kill(-leader, SIGUSR1) == 0, "kill_pgrp kill(-pgrp)", "must deliver");
    check(killpg(leader, SIGUSR1) == 0, "killpg_basic", "must deliver");
    for (int i = 2; i <= 4; i++)
        check(kill_wait_sem("ack", i, 3000) && kill_wait_sem("ack", i, 3000),
              "kill_pgrp member acknowledged twice", "missing delivery");
    /* kill(0, SIGUSR2) from the leader reaches the whole group. */
    kill_post("go", 2);
    for (int i = 2; i <= 4; i++)
        check(kill_wait_sem("ack", i, 3000), "kill_zero member acknowledged",
              "kill(0) must reach every group member");
    check(kill_reap(2, leader) == 21,
          "kill_pgrp exactly once per signal, all three members",
          "each member: two SIGUSR1 and one SIGUSR2");
    kill_cleanup(2, 4);
}


/* ---- siginfo sender and handler priority ---- */

static volatile pid_t si_got_pid;
static volatile int si_got_code;
static volatile int si_got_prio[2];
static volatile sig_atomic_t si_hits;

static void si_handler(int sig, siginfo_t *si, void *ctx)
{
    struct sched_param sp;
    int pol;

    (void) sig;
    (void) ctx;
    si_got_pid = si->si_pid;
    si_got_code = si->si_code;
    if (pthread_getschedparam(pthread_self(), &pol, &sp) == 0 && si_hits < 2)
        si_got_prio[si_hits] = sp.sched_priority;
    si_hits++;
}

static int si_install(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = si_handler;
    sa.sa_flags = SA_SIGINFO;
    return sigaction(SIGUSR1, &sa, NULL);
}

static void si_wait(int hits)
{
    for (int i = 0; i < 500 && si_hits < hits; i++)
        nap();
}

/* Child fixtures. --kill-sigsender pid: signal pid once and exit.
 * --kill-sigprio: install the SA_SIGINFO handler, announce ready, wait for two
 * signals, and exit 0 only if the handler ran at this process's own priority
 * both times (20, set by the spawner).
 */
static int si_sender(pid_t target)
{
    return kill(target, SIGUSR1) == 0 ? 0 : 1;
}

static int si_prio_child(void)
{
    sem_t *ready = kill_sem("sigready", 0, 0), *ack = kill_sem("sigack", 0, 0);

    if (ready == SEM_FAILED || ack == SEM_FAILED || si_install() != 0)
        return 100;
    (void) sem_post(ready);
    for (int n = 1; n <= 2; n++) {
        si_wait(n);
        (void) sem_post(ack);
    }
    return si_hits == 2 && si_got_prio[0] == 20 && si_got_prio[1] == 20
               ? 0
               : 1 + (si_got_prio[0] != 20) + 2 * (si_got_prio[1] != 20);
}

static pid_t si_spawn(const char *flag, const char *arg, int prio)
{
    char *argv[] = {"syscall_testing", (char *) flag, (char *) arg, NULL};
    posix_spawnattr_t attr;
    struct sched_param sp = {.sched_priority = prio};
    pid_t pid;
    int rc;

    posix_spawnattr_init(&attr);
    if (prio) {
        posix_spawnattr_setschedpolicy(&attr, SCHED_FIFO);
        posix_spawnattr_setschedparam(&attr, &sp);
        posix_spawnattr_setflags(
            &attr, POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSCHEDPARAM);
    }
    rc = posix_spawn(&pid, "/bin/syscall_testing", NULL, &attr, argv, NULL);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
}

static void test_si_raise(void)
{
    si_hits = 0;
    check(si_install() == 0, "si_raise install", "sigaction failed");
    (void) raise(SIGUSR1);
    check(si_hits == 1 && si_got_pid == getpid() && si_got_code == SI_USER,
          "si_raise: si_pid is the raiser, si_code SI_USER", "wrong siginfo");
}

static void test_si_pid_large(void)
{
    pid_t child;
    int status;

    /* Pids advance by 4096 per exit, so after a few spawns they exceed 2^15
     * (the old encoding kept 16 bits of sender pid).
     */
    for (int i = 0; i < 10; i++) {
        char *argv[] = {"true", NULL};
        pid_t p;

        if (posix_spawn(&p, "/bin/true", NULL, NULL, argv, NULL) == 0)
            (void) waitpid(p, &status, 0);
    }
    si_hits = 0;
    si_got_pid = 0;
    check(si_install() == 0, "si_pid_large install", "sigaction failed");
    char arg[16];

    snprintf(arg, sizeof arg, "%d", (int) getpid());
    child = si_spawn("--kill-sigsender", arg, 0);
    if (child < 0) {
        test_fail("si_pid_large spawn", strerror(errno));
        return;
    }
    (void) waitpid(child, &status, 0);
    si_wait(1);
    check(si_hits == 1 && si_got_pid == child && child > 0x7fff,
          "si_pid_large: si_pid equals a sender pid above 2^15",
          "sender pid was truncated or lost");
}

static void test_sigthread_prio_stable(void)
{
    pid_t child;
    int status = -1;

    kill_sem_create("sigready", 0);
    kill_sem_create("sigack", 0);
    child = si_spawn("--kill-sigprio", "x", 20);
    if (child < 0 || !kill_wait_sem("sigready", 0, 5000)) {
        test_fail("sigthread_prio_stable fixture", "child did not start");
        return;
    }
    (void) kill(child, SIGUSR1);
    (void) kill_wait_sem("sigack", 0, 3000);
    (void) kill(child, SIGUSR1);
    (void) kill_wait_sem("sigack", 0, 3000);
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "sigthread_prio_stable: handler stays at FIFO/20 on both deliveries",
          "handler priority changed after a signal");
    kill_sem_drop("sigready", 0);
    kill_sem_drop("sigack", 0);
}

/* ---- caught signal interrupts a blocked call with EINTR (Q35) ---- */

enum eintr_kind {
    EK_READ,
    EK_NANOSLEEP,
    EK_SLEEP,
    EK_SEM,
    EK_COND,
    EK_WAITPID
};

static struct {
    int kind;
    int fds[2];
    sem_t sem;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pid_t child;
    volatile int started;
    volatile int done;
    volatile long ret;
    volatile int err;
    volatile long rem_ns; /* nanosleep remainder, -1 when not reported */
    volatile unsigned sleep_rem;
    volatile char byte;
} ej;

static volatile sig_atomic_t eintr_hits;

static void eintr_handler(int sig)
{
    (void) sig;
    eintr_hits++;
}

static void eintr_install(int flags)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = eintr_handler;
    sa.sa_flags = flags;
    (void) sigaction(SIGUSR1, &sa, NULL);
    eintr_hits = 0;
}

static void eintr_uninstall(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_DFL;
    (void) sigaction(SIGUSR1, &sa, NULL);
}

static void *eintr_worker(void *arg)
{
    struct timespec ts = {.tv_sec = 30}, rem = {0, 0};
    char b = 0;

    (void) arg;
    ej.started = 1;
    errno = 0;
    switch (ej.kind) {
    case EK_READ:
        ej.ret = read(ej.fds[0], &b, 1);
        ej.byte = b;
        break;
    case EK_NANOSLEEP:
        ej.ret = nanosleep(&ts, &rem);
        ej.rem_ns = (long) rem.tv_sec * 1000000000L + rem.tv_nsec;
        break;
    case EK_SLEEP:
        ej.sleep_rem = sleep(30);
        ej.ret = 0;
        break;
    case EK_SEM:
        ej.ret = sem_wait(&ej.sem);
        break;
    case EK_COND:
        pthread_mutex_lock(&ej.mu);
        ej.ret = pthread_cond_wait(&ej.cv, &ej.mu);
        pthread_mutex_unlock(&ej.mu);
        break;
    case EK_WAITPID:
        ej.ret = waitpid(ej.child, NULL, 0);
        break;
    }
    ej.err = errno;
    __atomic_store_n(&ej.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static int eintr_wait_done(int ms)
{
    for (int i = 0; i < ms / 10 && !__atomic_load_n(&ej.done, __ATOMIC_ACQUIRE);
         i++)
        nap();
    return __atomic_load_n(&ej.done, __ATOMIC_ACQUIRE);
}

/* Start a worker of the given kind and wait until it is blocked in the call. */
static pthread_t eintr_start(int kind)
{
    pthread_t t;

    ej.started = 0;
    ej.done = 0;
    ej.ret = 0;
    ej.err = 0;
    ej.byte = 0;
    ej.sleep_rem = 0;
    ej.kind = kind;
    ej.rem_ns = -1;
    if (pthread_create(&t, NULL, eintr_worker, NULL) != 0)
        return 0;
    for (int i = 0; i < 100 && !ej.started; i++)
        nap();
    for (int i = 0; i < 10; i++)
        nap(); /* 100 ms: let it reach the kernel wait */
    return t;
}

static void eintr_reset_pipe(void)
{
    ej.fds[0] = ej.fds[1] = -1;
    (void) pipe(ej.fds);
}

static void eintr_close_pipe(void)
{
    close(ej.fds[0]);
    close(ej.fds[1]);
}

static void test_eintr_read_pipe(void)
{
    pthread_t t;

    eintr_reset_pipe();
    eintr_install(0);
    t = eintr_start(EK_READ);
    check(t != 0, "eintr_read_pipe fixture", "pthread_create failed");
    (void) kill(getpid(), SIGUSR1);
    check(eintr_wait_done(2000) && ej.ret == -1 && ej.err == EINTR,
          "eintr_read_pipe: caught SIGUSR1 breaks read() with EINTR",
          "read() was not interrupted");
    check(eintr_hits == 1, "eintr_read_pipe: handler ran once", "handler count");
    if (!ej.done)
        (void) write(ej.fds[1], "x", 1);
    if (t)
        pthread_join(t, NULL);
    eintr_uninstall();
    eintr_close_pipe();
}

static void test_eintr_nanosleep(void)
{
    pthread_t t;

    eintr_install(0);
    t = eintr_start(EK_NANOSLEEP);
    (void) kill(getpid(), SIGUSR1);
    check(eintr_wait_done(2000) && ej.ret == -1 && ej.err == EINTR,
          "eintr_nanosleep: nanosleep() returns EINTR", "not interrupted");
    check(ej.rem_ns > 0 && ej.rem_ns < 30000000000L,
          "eintr_nanosleep: remainder is set and shorter than the request",
          "bad remainder");
    if (t)
        pthread_join(t, NULL);

    t = eintr_start(EK_SLEEP);
    (void) kill(getpid(), SIGUSR1);
    check(eintr_wait_done(2000) && ej.sleep_rem > 0 && ej.sleep_rem <= 30,
          "eintr_sleep: sleep() returns the unslept seconds",
          "sleep() was not interrupted");
    if (t)
        pthread_join(t, NULL);
    eintr_uninstall();
}

static void test_eintr_sem_wait(void)
{
    pthread_t t;

    (void) sem_init(&ej.sem, 0, 0);
    eintr_install(0);
    t = eintr_start(EK_SEM);
    (void) kill(getpid(), SIGUSR1);
    check(eintr_wait_done(2000) && ej.ret == -1 && ej.err == EINTR,
          "eintr_sem_wait: sem_wait() returns EINTR", "not interrupted");
    if (!ej.done)
        (void) sem_post(&ej.sem);
    if (t)
        pthread_join(t, NULL);
    eintr_uninstall();
    (void) sem_destroy(&ej.sem);
}

/* pthread_cond_wait never reports EINTR: the interrupt is a spurious wakeup. */
static void test_eintr_cond_wait(void)
{
    pthread_t t;

    pthread_mutex_init(&ej.mu, NULL);
    pthread_cond_init(&ej.cv, NULL);
    eintr_install(0);
    t = eintr_start(EK_COND);
    (void) kill(getpid(), SIGUSR1);
    check(eintr_wait_done(2000) && ej.ret == 0,
          "eintr_cond_wait: spurious wakeup returns 0, not EINTR",
          "cond_wait stayed blocked or returned an error");
    if (!ej.done) {
        pthread_mutex_lock(&ej.mu);
        pthread_cond_signal(&ej.cv);
        pthread_mutex_unlock(&ej.mu);
    }
    if (t)
        pthread_join(t, NULL);
    eintr_uninstall();
}

/* SA_RESTART: the handler runs but the read keeps blocking and later gets data. */
static void test_restart_read(void)
{
    pthread_t t;

    eintr_reset_pipe();
    eintr_install(SA_RESTART);
    t = eintr_start(EK_READ);
    (void) kill(getpid(), SIGUSR1);
    for (int i = 0; i < 20 && eintr_hits == 0; i++)
        nap();
    check(eintr_hits == 1 && !ej.done,
          "restart_read: SA_RESTART handler ran and read() kept blocking",
          "read() returned early");
    (void) write(ej.fds[1], "z", 1);
    check(eintr_wait_done(2000) && ej.ret == 1 && ej.byte == 'z',
          "restart_read: read() later returns the data", "wrong result");
    if (t)
        pthread_join(t, NULL);
    eintr_uninstall();
    eintr_close_pipe();
}

/* A blocked signal is not delivered, so its waiter is not interrupted until the
 * signal is unblocked. (The mask is process-wide, see pthread-sigmask.c.)
 */
static void test_eintr_masked_thread(void)
{
    pthread_t t;
    sigset_t set;

    sigemptyset(&set);
    sigaddset(&set, SIGUSR1);
    eintr_reset_pipe();
    eintr_install(0);
    (void) sigprocmask(SIG_BLOCK, &set, NULL);
    t = eintr_start(EK_READ);
    (void) kill(getpid(), SIGUSR1);
    for (int i = 0; i < 10; i++)
        nap();
    check(!ej.done && eintr_hits == 0,
          "eintr_masked_thread: a blocked signal does not interrupt the waiter",
          "waiter was interrupted while the signal was blocked");
    (void) sigprocmask(SIG_UNBLOCK, &set, NULL);
    check(eintr_wait_done(2000) && ej.ret == -1 && ej.err == EINTR,
          "eintr_masked_thread: unblocking delivers it and interrupts the waiter",
          "waiter not interrupted after unblock");
    if (!ej.done)
        (void) write(ej.fds[1], "x", 1);
    if (t)
        pthread_join(t, NULL);
    eintr_uninstall();
    eintr_close_pipe();
}

/* One signal interrupts one waiter: the lowest tid. */
static int one_fds[2];
static volatile int one_ret[2], one_err[2], one_done[2];

static void *eintr_one_reader(void *arg)
{
    long i = (long) arg;
    char b;

    errno = 0;
    one_ret[i] = (int) read(one_fds[0], &b, 1);
    one_err[i] = errno;
    __atomic_store_n(&one_done[i], 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_eintr_one_waiter(void)
{
    pthread_t t[2];

    if (pipe(one_fds) != 0) {
        test_fail("eintr_one_waiter fixture", "pipe failed");
        return;
    }
    one_done[0] = one_done[1] = 0;
    eintr_install(0);
    pthread_create(&t[0], NULL, eintr_one_reader, (void *) 0L);
    for (int i = 0; i < 5; i++)
        nap();
    pthread_create(&t[1], NULL, eintr_one_reader, (void *) 1L);
    for (int i = 0; i < 10; i++)
        nap();
    (void) kill(getpid(), SIGUSR1);
    for (int i = 0; i < 200 && !one_done[0] && !one_done[1]; i++)
        nap();
    for (int i = 0; i < 10; i++)
        nap();
    check(one_done[0] && one_ret[0] == -1 && one_err[0] == EINTR && !one_done[1],
          "eintr_one_waiter: only the lowest-tid waiter returns EINTR",
          "wrong waiter interrupted or both returned");
    (void) write(one_fds[1], "x", 1);
    pthread_join(t[0], NULL);
    pthread_join(t[1], NULL);
    eintr_uninstall();
    close(one_fds[0]);
    close(one_fds[1]);
}

/* waitpid() blocked on a live child (a _PROC_WAIT message to roottask). */
static void test_eintr_waitpid(void)
{
    pthread_t t;
    pid_t child = kill_spawn(7, 0, -1);

    if (child < 0 || !kill_wait_sem("ready", 7, 5000)) {
        test_fail("eintr_waitpid fixture", "child did not start");
        kill_cleanup(7, 7);
        return;
    }
    eintr_install(0);
    ej.child = child;
    t = eintr_start(EK_WAITPID);
    (void) kill(getpid(), SIGUSR1);
    check(eintr_wait_done(2000) && ej.ret == -1 && ej.err == EINTR,
          "eintr_waitpid: waitpid() returns EINTR", "not interrupted");
    (void) kill_reap(7, child);
    if (t)
        pthread_join(t, NULL);
    eintr_uninstall();
    kill_cleanup(7, 7);
}

/* Cross-process: the child's handler runs on its system thread (the signal
 * arrives as a notification), and its blocked reader must still get EINTR.
 */
static void *eintr_remote_reader(void *arg)
{
    char b;

    errno = 0;
    ej.ret = read(ej.fds[0], &b, 1);
    ej.err = errno;
    __atomic_store_n(&ej.done, 1, __ATOMIC_RELEASE);
    return arg;
}

static int eintr_remote_child(void)
{
    sem_t *ready = kill_sem("eready", 0, 0);
    pthread_t t;

    if (ready == SEM_FAILED || pipe(ej.fds) != 0)
        return 100;
    eintr_install(0);
    if (pthread_create(&t, NULL, eintr_remote_reader, NULL) != 0)
        return 101;
    for (int i = 0; i < 100000; i++)
        sched_yield(); /* let the reader reach read() */
    (void) sem_post(ready);
    /* Spin, not nap(): a sleeping main thread would itself be a registered
     * waiter with a lower tid than the reader, and take the interrupt.
     */
    for (int i = 0; i < 5000000 && !__atomic_load_n(&ej.done, __ATOMIC_ACQUIRE);
         i++)
        sched_yield();
    if (!ej.done)
        return 102;
    return ej.ret == -1 && ej.err == EINTR && eintr_hits == 1 ? 0 : 103;
}

static void test_eintr_remote(void)
{
    char *argv[] = {"syscall_testing", "--kill-eintr", NULL};
    pid_t child;
    int status = -1;

    kill_sem_create("eready", 0);
    if (posix_spawn(&child, "/bin/syscall_testing", NULL, NULL, argv, NULL) !=
            0 ||
        !kill_wait_sem("eready", 0, 5000)) {
        test_fail("eintr_remote fixture", "child did not start");
        kill_sem_drop("eready", 0);
        return;
    }
    nap();
    (void) kill(child, SIGUSR1);
    (void) waitpid(child, &status, 0);
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        test_ok("eintr_remote: a signal from another process interrupts the reader");
    } else {
        char why[64];

        snprintf(why, sizeof why, "child status 0x%x (100+ = fixture step)",
                 (unsigned) status);
        test_fail("eintr_remote: a signal from another process interrupts the reader",
                  why);
    }
    kill_sem_drop("eready", 0);
}

/* thread_ctl(RTMUX_TCTL_INTERRUPT) refusals: it only breaks a thread of the
 * caller's own process that is in an interruptible wait.
 */
static void *eintr_mutex_waiter(void *arg)
{
    pthread_mutex_lock((pthread_mutex_t *) arg);
    pthread_mutex_unlock((pthread_mutex_t *) arg);
    __atomic_store_n(&ej.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_eintr_ctl_negative(void)
{
    pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_t t;

    errno = 0;
    check(thread_ctl(RTMUX_TCTL_INTERRUPT, (void *) 0L) == -1 && errno == EINVAL,
          "eintr_ctl: tid 0 is EINVAL", "wrong result");
    errno = 0;
    check(thread_ctl(RTMUX_TCTL_INTERRUPT, (void *) 100000L) == -1 &&
              errno == ESRCH,
          "eintr_ctl: an unknown tid (e.g. another process's) is ESRCH",
          "wrong result");
    errno = 0;
    check(thread_ctl(RTMUX_TCTL_INTERRUPT, (void *) (long) pthread_self()) ==
                  -1 &&
              errno == EAGAIN,
          "eintr_ctl: the running caller is EAGAIN", "wrong result");

    ej.done = 0;
    pthread_mutex_lock(&m);
    pthread_create(&t, NULL, eintr_mutex_waiter, &m);
    for (int i = 0; i < 10; i++)
        nap();
    errno = 0;
    check(thread_ctl(RTMUX_TCTL_INTERRUPT, (void *) (long) t) == -1 &&
              errno == EAGAIN && !ej.done,
          "eintr_ctl: a mutex wait is never interrupted (EAGAIN)",
          "mutex waiter was disturbed");
    pthread_mutex_unlock(&m);
    pthread_join(t, NULL);
}

/* SMP race check: one worker blocks and is interrupted 300 times while the
 * signaller races its wake against the block (at CPUS>1 they are on different
 * harts). A lost interrupt would hang the worker, a double wake would corrupt
 * the sem waitlist or hit the force_ready double-unlink guards.
 */
#define EINTR_STRESS_ITERS 300
static volatile int stress_count;

static void *eintr_stress_worker(void *arg)
{
    (void) arg;
    for (int i = 0; i < EINTR_STRESS_ITERS; i++) {
        if (sem_wait(&ej.sem) == -1 && errno == EINTR)
            stress_count++;
    }
    __atomic_store_n(&ej.done, 1, __ATOMIC_RELEASE);
    return NULL;
}

static void test_eintr_stress(void)
{
    pthread_t t;
    int sent = 0;

    (void) sem_init(&ej.sem, 0, 0);
    eintr_install(0);
    stress_count = 0;
    ej.done = 0;
    pthread_create(&t, NULL, eintr_stress_worker, NULL);
    while (!__atomic_load_n(&ej.done, __ATOMIC_ACQUIRE) && sent < 200000) {
        (void) kill(getpid(), SIGUSR1);
        sent++;
        sched_yield();
    }
    check(ej.done && stress_count == EINTR_STRESS_ITERS,
          "eintr_stress: every interrupted sem_wait returned EINTR",
          "lost or extra interrupt");
    if (!ej.done)
        for (int i = 0; i < EINTR_STRESS_ITERS; i++)
            (void) sem_post(&ej.sem);
    pthread_join(t, NULL);
    eintr_uninstall();
    (void) sem_destroy(&ej.sem);
}

/* A thread cancelled inside a wrapped call must leave the waiter list: a stale
 * record would be chosen as the lowest tid and starve the next real waiter.
 */
static void test_eintr_cancel_purge(void)
{
    pthread_t a, b;

    eintr_reset_pipe();
    eintr_install(0);
    ej.kind = EK_READ;
    ej.done = 0;
    ej.started = 0;
    pthread_create(&a, NULL, eintr_worker, NULL);
    for (int i = 0; i < 10; i++)
        nap();
    (void) pthread_cancel(a);
    pthread_join(a, NULL);
    check(!ej.done, "eintr_cancel_purge: cancelled reader never returned",
          "reader completed instead of being cancelled");

    b = eintr_start(EK_READ);
    (void) kill(getpid(), SIGUSR1);
    check(eintr_wait_done(2000) && ej.ret == -1 && ej.err == EINTR,
          "eintr_cancel_purge: next waiter is interrupted, not a stale record",
          "stale waiter swallowed the interrupt");
    if (!ej.done)
        (void) write(ej.fds[1], "x", 1);
    if (b)
        pthread_join(b, NULL);
    eintr_uninstall();
    eintr_close_pipe();
}

static void test_eintr(void)
{
    test_eintr_read_pipe();
    test_eintr_nanosleep();
    test_eintr_sem_wait();
    test_eintr_cond_wait();
    test_restart_read();
    test_eintr_masked_thread();
    test_eintr_one_waiter();
    test_eintr_waitpid();
    test_eintr_remote();
    test_eintr_ctl_negative();
    test_eintr_cancel_purge();
    test_eintr_stress();
}

void test_signals(void)
{
    test_sigaction_roundtrip();
    test_sigaction_uncatchable();
    test_raise();
    test_signal_returns_prev();
    test_sigprocmask_block();
    test_kill_self();
    test_pthread_sigmask();
    test_pthread_kill();
    test_signal_set_helpers();
    test_kill_cross_uid();
    test_kill_pgrp();
    test_si_raise();
    test_si_pid_large();
    test_sigthread_prio_stable();
    test_eintr();
    test_jobctl();
}
