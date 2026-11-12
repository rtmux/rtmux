/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * serial-unblock: a reader parked in dev-serial-8250 that dies must not strand
 * the driver. The harness (init + scripts/qemu-regress.sh) starts the driver,
 * then writes one byte to the UART after "serial-unblock: WAITING".
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
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

static void sleep_ms(long ms)
{
    struct timespec ts = {.tv_sec = ms / 1000,
                          .tv_nsec = (ms % 1000) * 1000000L};

    nanosleep(&ts, NULL);
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

    test_second_open_survives_close();

    a = spawn_reader(1);
    check(a > 0, "serial_unblock spawn parked reader", strerror(errno));
    if (a <= 0)
        return;
    sleep_ms(500); /* let it reach the driver and park */
    check(kill(a, SIGKILL) == 0, "serial_unblock kill parked reader", NULL);
    reaped = waitpid(a, &status, 0);
    snprintf(why, sizeof why, "pid=%d status=0x%x", (int) reaped, status);
    /* RTMux reports a SIGKILLed child as exit status 128 + SIGKILL. */
    check(reaped == a && (WIFSIGNALED(status) ||
                          (WIFEXITED(status) &&
                           WEXITSTATUS(status) == 128 + SIGKILL)),
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

    printf("serial-unblock: WAITING\n");
    fflush(stdout);
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

/* Own a foreground UART driver for an interactive coverage run. The parent
 * shell can resume using its polling console after the driver is reaped. */
void test_serial_unblock_driver(const char *driver)
{
    char *argv[] = {(char *) driver, NULL};
    pid_t pid = -1;
    int status = 0, ready = 0;
    int rc = posix_spawn(&pid, driver, NULL, NULL, argv, NULL);
    check(rc == 0 && pid > 0, "serial_unblock spawn driver", strerror(rc));
    if (rc != 0 || pid <= 0)
        return;
    for (int i = 0; i < 500; i++) {
        if (access(SERIAL_DEV, F_OK) == 0) {
            ready = 1;
            break;
        }
        sleep_ms(10);
    }
    check(ready, "serial_unblock driver published", "UART service missing");
    if (ready)
        test_serial_unblock();
    rc = kill(pid, SIGKILL);
    check(rc == 0 && waitpid(pid, &status, 0) == pid,
          "serial_unblock driver reaped", "UART driver must release input ownership");
}
