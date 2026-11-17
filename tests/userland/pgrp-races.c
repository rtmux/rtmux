/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Process-group races, each made certain by holding open a window in roottask.
 *
 * A CONFIG_DEBUG_INJECT roottask can hold the next setpgid, or the next
 * terminal detach, for a few hundred milliseconds at the point where the
 * operation has checked and not yet changed (_PROC_JOBCTL_DEBUG). The test
 * starts a second operation inside that window, which is otherwise a matter of
 * luck, and checks what a serial order of the two would have given:
 *
 * - setpgid against the last member's reap: a process may not join a group
 *   whose last member is reaped between the check and the join (EPERM, and no
 *   process left in a group that does not exist).
 * - tcsetsid(-1) against a rebind: the detach's message to the terminal server
 *   must not run after the rebind's and clear the new binding.
 *
 * Two cases need no window. A group is its members, a zombie included, until
 * the last is reaped (so a zombie leader's group can still be joined); a
 * session's terminal server forgets its session when the session's last member
 * is reaped; and setpgid on a child that has exec'd is EACCES. Without the
 * injection call the window cases are skipped.
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <devctl.h>
#include <devctl/tty.h>
#include <sys/procmsg.h>
#include <sys/rtmux-core.h>
#include <sys/wait.h>

#include "outcome.h"
#include "test.h"

#define PGW_MS 600

#define TTY_PATH "/dev/ser1" /* a terminal the starting session does not own */

/* Arm a window in roottask; false when this roottask has none. */
static bool pgw_arm(unsigned where, unsigned ms)
{
    struct _proc_jobctl msg;

    memset(&msg, 0, sizeof msg);
    msg.type = _PROC_JOBCTL;
    msg.subtype = _PROC_JOBCTL_DEBUG;
    msg.arg = (int32_t) where;
    msg.arg2 = (int32_t) ms;
    return msg_sendnc(ROOTTASK_COID, &msg, sizeof msg, 0, 0) == 0;
}

/* Wait up to five seconds for a byte on fd (non-blocking read); false when
 * none came, or the writers are gone.
 */
static bool wait_byte(int fd, char *c)
{
    (void) fcntl(fd, F_SETFL, O_NONBLOCK);
    for (int i = 0; i < 250; i++) {
        ssize_t n = read(fd, c, 1);

        if (n == 1)
            return true;
        if (n == 0 || (n == -1 && errno != EAGAIN))
            return false;
        sleep_ms(20);
    }
    return false;
}

/* Spawn "syscall_testing mode" with a pipe on its stdin, which it stays alive
 * on until *hold is closed, and one on its stdout, whose read end is *rfd (or
 * closed, if rfd is NULL). Returns its pid, or -1.
 */
static pid_t pipe_spawn(const char *mode, int *hold, int *rfd)
{
    char *argv[] = {"syscall_testing", (char *) mode, NULL};
    posix_spawn_file_actions_t fa;
    int in[2], out[2];
    pid_t pid = (pid_t) -1;
    int rc;

    *hold = -1;
    if (rfd)
        *rfd = -1;
    if (pipe(in) != 0)
        return -1;
    if (pipe(out) != 0) {
        (void) close(in[0]);
        (void) close(in[1]);
        return -1;
    }
    /* Only this child may hold the write end, or its EOF never comes. */
    (void) fcntl(in[1], F_SETFD, FD_CLOEXEC);
    (void) fcntl(out[0], F_SETFD, FD_CLOEXEC);
    (void) posix_spawn_file_actions_init(&fa);
    (void) posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    (void) posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    (void) posix_spawn_file_actions_addclose(&fa, in[0]);
    (void) posix_spawn_file_actions_addclose(&fa, out[1]);
    rc = posix_spawn(&pid, "/bin/syscall_testing", &fa, NULL, argv, NULL);
    (void) posix_spawn_file_actions_destroy(&fa);
    (void) close(in[0]);
    (void) close(out[1]);
    if (rc != EOK) {
        (void) close(in[1]);
        (void) close(out[0]);
        return -1;
    }
    *hold = in[1];
    if (rfd)
        *rfd = out[0];
    else
        (void) close(out[0]);
    return pid;
}

/* Spawn a child that says it is running, then stays until *hold is closed.
 * Returns its pid, or -1.
 */
static pid_t hold_spawn(const char *mode, int *hold)
{
    int rfd;
    char c;
    pid_t pid = pipe_spawn(mode, hold, &rfd);

    if (pid == (pid_t) -1)
        return pid;
    if (!wait_byte(rfd, &c)) {
        (void) kill(pid, SIGKILL);
        (void) close(*hold);
        (void) waitpid(pid, &(int) {0}, 0);
        pid = (pid_t) -1;
    }
    (void) close(rfd);
    return pid;
}

/* Let a hold_spawn() child exit and reap it. */
static void hold_release(pid_t pid, int hold)
{
    int st;

    if (pid == (pid_t) -1)
        return;
    (void) close(hold);
    (void) waitpid(pid, &st, 0);
}

/* A child that execs: it replaces itself with a --getsid-hold, whose pipes
 * are its own stdin and stdout.
 */
int pgrp_exec_hold(void)
{
    char *argv[] = {"syscall_testing", "--getsid-hold", NULL};

    (void) execv("/bin/syscall_testing", argv);
    return 2; /* the exec failed */
}

/* setpgid on a child that has exec'd is EACCES; on one that has only been
 * spawned, it works. The child has exec'd once it says so from its new image,
 * or, where an exec does not start the new image (see below), once it has
 * gone: either way setpgid must refuse it. Only a child that has exited and
 * not been collected can be looked at without a waiter, which is not queued
 * for the exit, so the look is WNOHANG.
 *
 * In this tree a successful exec replaces the caller and the new image is
 * never started, because nothing consumes the loader state the exec leaves
 * (roottask's _PROC_SPAWN_EXEC sets it and the process is torn down): the
 * exec'd process exits with status 0 and says nothing, which is why this case
 * cannot wait for the new image.
 */
static void pgrp_exec_eacces(void)
{
    int h_exec, h_plain, rfd;
    pid_t c = pipe_spawn("--pgrp-exec-hold", &h_exec, &rfd);
    pid_t p = hold_spawn("--getsid-hold", &h_plain);
    siginfo_t si;
    bool execd = false;
    char b;
    int rc, err;

    check(c != (pid_t) -1 && p != (pid_t) -1, "pgrp exec: spawn children",
          "the case needs a spawned child and one that execs");
    if (c != (pid_t) -1) {
        (void) fcntl(rfd, F_SETFL, O_NONBLOCK);
        for (int i = 0; i < 250 && !execd; i++) {
            memset(&si, 0, sizeof si);
            execd = read(rfd, &b, 1) == 1 ||
                    (waitid(P_PID, (id_t) c, &si,
                            WEXITED | WNOHANG | WNOWAIT) == 0 &&
                     si.si_pid == c);
            if (!execd)
                sleep_ms(20);
        }
        check(execd, "pgrp exec: the child has exec'd",
              "the exec must have taken effect, or the child gone");
        errno = 0;
        rc = setpgid(c, c);
        err = errno;
        check(rc == -1 && err == EACCES, "setpgid after the child's exec",
              "a child that has exec'd must be refused with EACCES");
        (void) close(rfd);
    }
    if (p != (pid_t) -1) {
        check(setpgid(p, p) == 0 && getpgid(p) == p,
              "setpgid of a spawned child that has not exec'd",
              "a child that has only been spawned stays movable");
    }
    hold_release(c, h_exec);
    hold_release(p, h_plain);
}

/* A group is its members until the last is reaped, a zombie included: the
 * leader exits and is not yet collected, and the group can still be joined.
 */
static void pgrp_zombie_member(void)
{
    int hx, hy;
    pid_t x = hold_spawn("--getsid-hold", &hx);
    pid_t y = hold_spawn("--getsid-hold", &hy);
    siginfo_t si;
    int rc;

    check(x != (pid_t) -1 && y != (pid_t) -1, "pgrp zombie: spawn children",
          "the case needs two children");
    if (x == (pid_t) -1 || y == (pid_t) -1)
        goto out;
    check(setpgid(x, x) == 0, "pgrp zombie: the leader leads a group",
          "setpgid of a spawned child into its own group");
    /* Let it exit, and look for that without collecting it. A waiter queued
     * before the exit is answered by it and the child reaped, so wait first.
     */
    (void) close(hx);
    hx = -1;
    for (int i = 0; i < 100; i++) {
        sleep_ms(20);
        memset(&si, 0, sizeof si);
        rc = waitid(P_PID, (id_t) x, &si, WEXITED | WNOHANG | WNOWAIT);
        if (rc == 0 && si.si_pid == x)
            break;
    }
    check(rc == 0 && si.si_pid == x, "pgrp zombie: the leader is a zombie",
          "waitid WNOWAIT must see the exit without collecting it");
    rc = setpgid(y, x);
    check(rc == 0 && getpgid(y) == x,
          "setpgid joins a group whose leader is a zombie",
          "a zombie is still a member of its group");
    (void) waitpid(x, &(int) {0}, 0);
    x = (pid_t) -1;
out:
    if (hx >= 0)
        hold_release(x, hx);
    else if (x != (pid_t) -1)
        (void) waitpid(x, &(int) {0}, 0);
    hold_release(y, hy);
}

struct join_arg {
    pid_t who, group;
    int rc, err;
};

static void *join_thread(void *p)
{
    struct join_arg *a = p;

    errno = 0;
    a->rc = setpgid(a->who, a->group);
    a->err = errno;
    return NULL;
}

/* setpgid joins a group inside the window the last member's reap lands in: a
 * serial order has the reap first (EPERM) or the join first (it works, and the
 * group lives on in the process that joined). The window holds the join until
 * the reap is done, so only the first is right.
 */
static void pgrp_join_vs_reap(void)
{
    int hx, hm, hy;
    pid_t x = hold_spawn("--getsid-hold", &hx);
    pid_t m = hold_spawn("--getsid-hold", &hm);
    pid_t y = hold_spawn("--getsid-hold", &hy);
    struct join_arg a = {0};
    pthread_t th;
    char detail[96];

    check(x != (pid_t) -1 && m != (pid_t) -1 && y != (pid_t) -1,
          "pgrp join race: spawn children", "the case needs three children");
    if (x == (pid_t) -1 || m == (pid_t) -1 || y == (pid_t) -1)
        goto out;
    /* Group x: the child it is named for, then m alone once x is gone. */
    check(setpgid(x, x) == 0 && setpgid(m, x) == 0 && getpgid(m) == x,
          "pgrp join race: group of two", "m must join x's group");
    hold_release(x, hx);
    x = (pid_t) -1;
    a.who = y;
    a.group = getpgid(m);
    if (!pgw_arm(PGRP_WINDOW_SETPGID, PGW_MS)) {
        test_skip("pgrp join race", "no debug window in this roottask");
        goto out;
    }
    if (pthread_create(&th, NULL, join_thread, &a) != 0) {
        test_fail("pgrp join race", "pthread_create");
        goto out;
    }
    sleep_ms(PGW_MS / 3); /* the join is held in its window by now */
    hold_release(m, hm);
    m = (pid_t) -1;
    (void) pthread_join(th, NULL);
    snprintf(detail, sizeof detail,
             "joined a group whose last member was reaped: rc %d errno %d "
             "group %d",
             a.rc, a.err, (int) getpgid(y));
    check(a.rc == -1 && a.err == EPERM, "setpgid vs the last member's reap",
          detail);
out:
    hold_release(x, hx);
    hold_release(m, hm);
    hold_release(y, hy);
}

/* The session leader child of the terminal cases. It binds its session to the
 * terminal, then detaches and binds again with a second thread in between, and
 * exits 0 when the terminal server still names the session afterwards.
 */
struct tty_arg {
    int fd;
    pid_t sid;
    int rc;
};

static void *detach_thread(void *p)
{
    struct tty_arg *a = p;

    a->rc = tcsetsid(-1, a->sid);
    return NULL;
}

static pid_t tty_server_sid(int fd)
{
    pid_t sid = -1;

    if (devctl(fd, DEVCTL_TTY_GET_SID, &sid, sizeof sid, NULL) != EOK)
        return -1;
    return sid;
}

/* Exit codes of pgrp_tty_child. */
enum {
    TTY_OK = 0,
    TTY_NOSETSID = 10,
    TTY_NOTERMINAL = 11, /* the terminal is not free to bind: skip */
    TTY_NOWINDOW = 12,
    TTY_NOTBOUND = 13,
    TTY_DETACH = 14,
    TTY_REBIND = 15,
    TTY_CLEARED = 16 /* the rebind was cleared by the detach: the race */
};

int pgrp_tty_child(int race)
{
    struct tty_arg a;
    pthread_t th;
    int fd1, fd2;

    if (setsid() == -1)
        return TTY_NOSETSID;
    a.sid = getpid();
    fd1 = open(TTY_PATH, O_RDWR | O_NOCTTY);
    fd2 = open(TTY_PATH, O_RDWR | O_NOCTTY);
    if (fd1 < 0 || fd2 < 0 || tcsetsid(fd1, a.sid) != 0)
        return TTY_NOTERMINAL;
    if (tty_server_sid(fd1) != a.sid)
        return TTY_NOTBOUND;
    if (!race)
        return TTY_OK; /* leave it bound: the last member's reap unbinds it */
    if (!pgw_arm(PGRP_WINDOW_DETACH, PGW_MS))
        return TTY_NOWINDOW;
    a.fd = fd1;
    a.rc = -2;
    if (pthread_create(&th, NULL, detach_thread, &a) != 0)
        return TTY_DETACH;
    sleep_ms(PGW_MS / 3); /* the detach has let go of the terminal, and waits */
    if (tcsetsid(fd2, a.sid) != 0) {
        (void) pthread_join(th, NULL);
        return TTY_REBIND;
    }
    (void) pthread_join(th, NULL);
    if (a.rc != 0)
        return TTY_DETACH;
    return tty_server_sid(fd2) == a.sid ? TTY_OK : TTY_CLEARED;
}

static int tty_run(const char *mode, int *rc)
{
    char *argv[] = {"syscall_testing", (char *) mode, NULL};
    pid_t pid;
    int st;

    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL) != 0)
        return -1;
    if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
        return -1;
    *rc = WEXITSTATUS(st);
    return 0;
}

/* tcsetsid(-1) against a rebind, and the terminal server's memory of a session
 * after its last member is reaped.
 */
static void pgrp_tty(void)
{
    int fd = open(TTY_PATH, O_RDWR | O_NOCTTY);
    int rc = -1;
    pid_t sid = -1;

    if (fd < 0) {
        test_skip("pgrp terminal", "no serial terminal to bind");
        return;
    }
    if (tty_run("--pgrp-tty-bind", &rc) != 0 || rc != TTY_OK) {
        if (rc == TTY_NOTERMINAL || rc == TTY_NOTBOUND)
            test_skip("pgrp terminal", "the terminal is not free to bind");
        else
            test_fail("pgrp terminal: bind", "the session leader child failed");
        (void) close(fd);
        return;
    }
    /* The child is reaped; its session's terminal has been let go of by the
     * time the slot is. Teardown follows the wait, so look for it.
     */
    for (int i = 0; i < 100; i++) {
        if ((sid = tty_server_sid(fd)) == 0)
            break;
        sleep_ms(20);
    }
    check(sid == 0, "terminal server forgets a reaped session",
          "tcgetsid on an open descriptor must stop naming a dead session");
    (void) close(fd);

    if (tty_run("--pgrp-tty-race", &rc) != 0) {
        test_fail("tcsetsid vs rebind", "the session leader child did not run");
        return;
    }
    if (rc == TTY_NOWINDOW) {
        test_skip("tcsetsid vs rebind", "no debug window in this roottask");
        return;
    }
    check(rc == TTY_OK, "tcsetsid(-1) against a rebind",
          rc == TTY_CLEARED ? "the detach cleared the rebind's binding"
                            : "the session leader child failed");
}

int pgrp_races(void)
{

    if (!pgw_arm(0, 0))
        return TC_EXIT_SKIP;
    pgrp_zombie_member();
    pgrp_join_vs_reap();
    pgrp_tty();
    pgrp_exec_eacces();
    return 0;
}
