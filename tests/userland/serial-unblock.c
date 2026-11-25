/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * serial-unblock: a reader parked in dev-serial-8250 that dies must not strand
 * the driver. The harness (init + scripts/qemu-regress.sh) starts the driver,
 * and writes to the UART each byte the guest asks for with serial_inject().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/poll.h>
#include <signal.h>
#include <sched.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "test.h"

#define SERIAL_DEV "/dev/ser1"
#define SERIAL_BYTE 'x'
#define SERIAL_ERR_EXIT 100 /* child open()/read() failed */

void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
}

/* Foreground job for the single-hart serial preemption test. On a missed
 * interrupt the watchdog returns a normal status, distinguishable from SIGINT.
 */
int serial_priority_spinner(void)
{
    struct sched_param sp = {.sched_priority = 15};
    struct timespec start, now;
    /* The shell hands the terminal to the job after posix_spawn returns. */
    pid_t group = getpgrp();
    for (int i = 0; tcgetpgrp(STDIN_FILENO) != group; ++i) {
        if (i == 1000)
            return 1;
        sleep_ms(1);
    }
    if (sched_setscheduler(0, SCHED_FIFO, &sp) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &start) != 0)
        return 1;
    puts("serial priority spinner ready");
    fflush(stdout);
    do {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
            return 1;
    } while (now.tv_sec - start.tv_sec < 5);
    return 2;
}

/* Child: park one raw read on the serial device. Exits with the byte read, or
 * SERIAL_ERR_EXIT (open) / SERIAL_ERR_EXIT + 1 (read) on failure.
 */
int serial_unblock_child(int announce)
{
    char c;
    int fd = open(SERIAL_DEV, O_RDWR);

    if (fd < 0)
        return SERIAL_ERR_EXIT;
    if (announce) {
        printf("serial-unblock: PARKED\n");
        fflush(stdout);
    }
    if (read(fd, &c, 1) != 1) {
        printf("serial-unblock: read failed errno=%d\n", errno);
        fflush(stdout);
        return SERIAL_ERR_EXIT + 1;
    }
    return (unsigned char) c;
}

/* Ask the harness to write byte c to the UART. It reads the request from the
 * console log, so the byte arrives within about a second.
 */
static void inject(const char *tag, int *seq, int c)
{
    printf("%s-inject: %d %02x\n", tag, (*seq)++, (unsigned) (unsigned char) c);
    fflush(stdout);
}

void serial_inject(int c)
{
    static int seq;

    inject("serial", &seq, c);
}

/* The same for a byte meant for the console (/dev/text), asked for before the
 * serial driver starts. Its sequence is separate, because it is asked for by a
 * separate syscall_testing run.
 */
void console_inject(int c)
{
    static int seq;

    inject("console", &seq, c);
}

static pid_t spawn_reader(int announce)
{
    char *argv[] = {"syscall_testing", "--serial-unblock-child",
                    announce ? "announce" : "quiet", NULL};
    pid_t pid;

    if (posix_spawn(&pid, "/bin/syscall_testing", NULL, NULL, argv, NULL))
        return -1;
    return pid;
}

/* Poll for exit for up to ms; returns the waitpid() result (0 = running). */
static pid_t wait_for(pid_t pid, int *status, long ms)
{
    pid_t r;

    for (; (r = waitpid(pid, status, WNOHANG)) == 0 && ms > 0; ms -= 10)
        sleep_ms(10);
    return r;
}

/* A competing getty must give up while the first reader is parked. */
static void test_getty_busy(void)
{
    char *argv[] = {"getty", SERIAL_DEV, NULL};
    pid_t pid;
    int status;
    int rc = posix_spawn(&pid, "/sbin/getty", NULL, NULL, argv, NULL);
    if (rc) {
        test_fail("getty on busy serial line", strerror(rc));
        return;
    }
    pid_t done = wait_for(pid, &status, 3000);
    check(done == pid && WIFEXITED(status) && WEXITSTATUS(status) == 1,
          "getty exits on busy serial line", "expected exit status 1");
    if (!done) {
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
    }
}

/* Keep output on the console so the runner can count banners and errors. */
static void test_getty_hangup(int fd, const struct termios *saved)
{
    struct termios hung = *saved;
    posix_spawn_file_actions_t fa;
    char *argv[] = {"getty", NULL};
    int status, rc;
    pid_t pid;

    if (cfsetospeed(&hung, B0) != 0 || tcsetattr(fd, TCSANOW, &hung) != 0) {
        test_fail("getty set B0", strerror(errno));
        return;
    }
    puts("getty B0 begin");
    fflush(stdout);
    rc = posix_spawn_file_actions_init(&fa);
    if (!rc) {
        rc = posix_spawn_file_actions_adddup2(&fa, fd, STDIN_FILENO);
        if (!rc)
            rc = posix_spawn(&pid, "/sbin/getty", &fa, NULL, argv, NULL);
        posix_spawn_file_actions_destroy(&fa);
    }
    if (rc) {
        test_fail("getty hangup spawn", strerror(rc));
    } else {
        sleep_ms(2250);
        check(kill(pid, SIGKILL) == 0, "getty waits on hung-up line", NULL);
        waitpid(pid, &status, 0);
    }
    puts("getty B0 end");
    check(tcsetattr(fd, TCSANOW, saved) == 0, "getty restore serial line", NULL);
}

static volatile int second_done;
static volatile int second_rc;

/* Park one read on the second open of the device. */
static void *second_open_reader(void *arg)
{
    char c;

    second_rc = (int) read(*(int *) arg, &c, 1);
    second_done = 1;
    return NULL;
}

/* A read parked on one open must survive the close of another open of the same
 * process, and must still be released by the close of its own open.
 */
static void test_second_open_survives_close(void)
{
    pthread_t th;
    int fd1 = open(SERIAL_DEV, O_RDWR), fd2 = open(SERIAL_DEV, O_RDWR), i;

    if (fd1 < 0 || fd2 < 0) {
        test_fail("serial_unblock second open", strerror(errno));
        goto out;
    }
    second_done = 0;
    if (pthread_create(&th, NULL, second_open_reader, &fd2) != 0) {
        test_fail("serial_unblock second-open reader thread", strerror(errno));
        goto out;
    }
    sleep_ms(500); /* let it park */
    close(fd1);
    fd1 = -1;
    sleep_ms(500);
    check(!second_done, "serial_unblock close of first open keeps second open parked",
          "read on the second open was dropped by the first open's close");
    close(fd2); /* its own open: must release the reader */
    fd2 = -1;
    for (i = 0; i < 200 && !second_done; i++)
        sleep_ms(10);
    check(second_done, "serial_unblock close of own open releases parked read",
          "reader still blocked after its open was closed");
    if (second_done)
        pthread_join(th, NULL);
out:
    if (fd1 >= 0)
        close(fd1);
    if (fd2 >= 0)
        close(fd2);
}

void test_serial_unblock(void)
{
    struct termios t;
    char why[32];
    int fd, status = 0, early;
    pid_t a, b, reaped;

    printf("[serial-unblock]\n");
    fd = open(SERIAL_DEV, O_RDWR);
    if (fd < 0) {
        test_fail("serial_unblock open", strerror(errno));
        return;
    }
    char terminal[64];
    check(ttyname_r(fd, terminal, sizeof terminal) == 0 &&
              !strcmp(terminal, SERIAL_DEV),
          "ttyname matches serial line", NULL);
    /* Raw, no echo: one injected byte completes a read. */
    if (tcgetattr(fd, &t) != 0) {
        test_fail("serial_unblock tcgetattr", strerror(errno));
        return;
    }
    t.c_lflag &= ~(tcflag_t) (ICANON | ECHO);
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        test_fail("serial_unblock tcsetattr", strerror(errno));
        return;
    }

    test_tty_discipline(fd, serial_inject);
    test_getty_hangup(fd, &t);
    test_second_open_survives_close();

    a = spawn_reader(1);
    check(a > 0, "serial_unblock spawn parked reader", strerror(errno));
    if (a <= 0)
        return;
    sleep_ms(500); /* let it reach the driver and park */
    test_getty_busy();
    check(kill(a, SIGKILL) == 0, "serial_unblock kill parked reader", NULL);
    reaped = waitpid(a, &status, 0);
    snprintf(why, sizeof why, "pid=%d status=0x%x", (int) reaped, status);
    check(reaped == a && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL,
          "serial_unblock reap parked reader", why);

    b = spawn_reader(0);
    check(b > 0, "serial_unblock spawn replacement", strerror(errno));
    if (b <= 0)
        return;
    early = wait_for(b, &status, 1000) != 0; /* EBUSY would exit at once */
    check(!early, "serial_unblock replacement stays blocked (no EBUSY)",
          "replacement exited before any byte");
    if (early)
        return;

    serial_inject(SERIAL_BYTE);
    check(wait_for(b, &status, 20000) == b && WIFEXITED(status) &&
              WEXITSTATUS(status) == SERIAL_BYTE,
          "serial_unblock replacement got the injected byte",
          "byte lost to the dead reader, or wrong byte");
    if (waitpid(b, &status, WNOHANG) == 0) {
        kill(b, SIGKILL);
        waitpid(b, &status, 0);
    }
    close(fd);
}

static pid_t tty_program(const char *name, int tty, int *output)
{
    int fds[2], err;
    pid_t pid = -1;
    posix_spawn_file_actions_t fa;
    char *argv[] = {(char *) name, NULL};
    if (pipe(fds) < 0)
        return -1;
    err = posix_spawn_file_actions_init(&fa);
    if (!err) {
        if (!(err = posix_spawn_file_actions_adddup2(&fa, tty, 0)) &&
            !(err = posix_spawn_file_actions_adddup2(&fa, fds[1], 1)) &&
            !(err = posix_spawn_file_actions_addclose(&fa, fds[0])) &&
            !(err = posix_spawn_file_actions_addclose(&fa, fds[1])))
            err = posix_spawn(&pid, name, &fa, NULL, argv, NULL);
        posix_spawn_file_actions_destroy(&fa);
    }
    close(fds[1]);
    if (err) {
        close(fds[0]);
        errno = err;
        return -1;
    }
    *output = fds[0];
    return pid;
}

void test_tty_discipline(int fd, void (*inject_byte)(int))
{
    struct termios saved, t;
    int out, status;
    char data[128];
    if (tcgetattr(fd, &saved) < 0) {
        test_fail("tty discipline setup", strerror(errno));
        return;
    }
    t = saved;
    t.c_lflag |= ICANON | ECHOE;
    t.c_lflag &= ~(tcflag_t) ECHO;
    t.c_cc[VERASE] = 8;
    t.c_cc[VEOF] = 4;
    if (tcsetattr(fd, TCSAFLUSH, &t) < 0) {
        test_fail("tty canonical setup", strerror(errno));
        return;
    }
    pid_t pid = tty_program("/bin/cat", fd, &out);
    if (pid > 0) {
        static const char input[] = "abc\004\004";
        for (size_t i = 0; i < sizeof input - 1; i++)
            inject_byte(input[i]);
        int done = wait_for(pid, &status, 5000) == pid;
        if (!done) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
        }
        ssize_t n = read(out, data, sizeof data);
        check(done && WIFEXITED(status) && !WEXITSTATUS(status) && n == 3 &&
                  !memcmp(data, "abc", 3),
              "cat exits after abc^D^D",
              "expected one partial line followed by EOF");
        close(out);
    } else
        test_fail("spawn terminal cat", strerror(errno));

    static const char edit[] = "ab\bc\n";
    for (size_t i = 0; i < sizeof edit - 1; i++)
        inject_byte(edit[i]);
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    check(poll(&pfd, 1, 4000) == 1 && read(fd, data, sizeof data) == 3 &&
              !memcmp(data, "ac\n", 3),
          "canonical erase keeps ac", "expected ac and newline");

    pid = tty_program("/bin/vi", fd, &out);
    if (pid > 0) {
        struct pollfd paint = {.fd = out, .events = POLLIN};
        size_t used = 0;
        int drew = 0;
        while (used + 1 < sizeof data && poll(&paint, 1, 3000) == 1) {
            ssize_t n = read(out, data + used, sizeof data - 1 - used);
            if (n <= 0)
                break;
            used += (size_t) n;
            data[used] = 0;
            char *query = strstr(data, "\033[6n");
            if (query) {
                drew = query[4] || (poll(&paint, 1, 999) == 1 &&
                                    read(out, data, sizeof data) > 0);
                break;
            }
        }
        check(drew, "vi paints after unanswered size query",
              "no repaint within one second");
        kill(pid, SIGKILL);
        waitpid(pid, &status, 0);
        close(out);
    } else
        test_fail("spawn terminal vi", strerror(errno));
    (void) tcsetattr(fd, TCSAFLUSH, &saved);
}
