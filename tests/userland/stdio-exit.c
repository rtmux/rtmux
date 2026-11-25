/*
 * exit() and stdio: what exit() flushes, and what it may wait for.
 *
 * Each case runs in a child (syscall_testing --stdio-exit-child CASE) whose
 * stdin and stdout are pipes from the parent. The parent never writes to the
 * child's stdin, so a read of it blocks for as long as the child lives.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/wait.h>

#include "test.h"

#define SE_EXIT_MS 5000 /* a child that has not exited by then is stuck */

static atomic_int se_locked; /* the helper thread holds its stream */

static void *se_reader(void *arg)
{
    char line[64];

    (void) arg;
    /* Hold stdin's lock first, so the parent's exit() is known to find it
     * held, then wait for input that never comes (the lock is recursive).
     */
    flockfile(stdin);
    atomic_store(&se_locked, 1);
    (void) fgets(line, sizeof line, stdin);
    funlockfile(stdin);
    return NULL;
}

enum { SE_BULK = 256 * 1024 }; /* more than the pipe holds */

static void *se_bulk_writer(void *arg)
{
    static char bulk[SE_BULK];

    (void) arg;
    memset(bulk, 'x', sizeof bulk);
    /* Hold stdout across a write the pipe cannot take at once, so the
     * parent's exit() finds the stream held with _p at the buffer's base.
     */
    flockfile(stdout);
    atomic_store(&se_locked, 1);
    (void) fwrite(bulk, 1, sizeof bulk, stdout);
    (void) fflush(stdout);
    funlockfile(stdout);
    return NULL;
}

static void *se_writer(void *arg)
{
    (void) arg;
    /* Hold stdout, which has output in its buffer, across the exit() and let
     * go of it later: exit() has to wait for it and flush.
     */
    flockfile(stdout);
    atomic_store(&se_locked, 1);
    sleep_ms(300);
    funlockfile(stdout);
    return NULL;
}

/* Child side. Writes only through stdio, and touches no stream before the
 * case's first call, so a case sees the library's state at process start.
 */
int stdio_exit_child(const char *what)
{
    static char buf[4096];

    if (!strcmp(what, "setvbuf")) {
        /* Full buffering with a caller buffer before any other stdio use. */
        if (setvbuf(stdout, buf, _IOFBF, sizeof buf) != 0)
            return 2;
        fputs("stdio-exit setvbuf\n", stdout);
        return 0; /* exit() must flush it */
    }
    if (!strcmp(what, "held")) {
        pthread_t t;

        fputs("stdio-exit held\n", stdout); /* a pipe: fully buffered, not yet out */
        if (pthread_create(&t, NULL, se_reader, NULL) != 0)
            return 2;
        while (!atomic_load(&se_locked))
            sleep_ms(1);
        sleep_ms(50); /* and let it block inside fgets */
        return 0;     /* exit() while another thread holds stdin */
    }
    if (!strcmp(what, "bulk")) {
        pthread_t t;

        if (pthread_create(&t, NULL, se_bulk_writer, NULL) != 0)
            return 2;
        while (!atomic_load(&se_locked))
            sleep_ms(1);
        sleep_ms(20); /* let it block in the write */
        return 0;     /* exit() while it is writing */
    }
    if (!strcmp(what, "writer")) {
        pthread_t t;

        fputs("stdio-exit writer\n", stdout); /* pending in stdout's buffer */
        if (pthread_create(&t, NULL, se_writer, NULL) != 0)
            return 2;
        while (!atomic_load(&se_locked))
            sleep_ms(1);
        return 0; /* exit() while another thread holds stdout */
    }
    return 2;
}

/* Drain what the child has written so far: the first len - 1 bytes into out,
 * all of them counted in *total. Returns when the pipe is empty or closed.
 */
static void se_drain(int fd, char *out, size_t len, size_t *total)
{
    char chunk[512];

    for (;;) {
        ssize_t n = read(fd, chunk, sizeof chunk);

        if (n <= 0)
            return;
        for (ssize_t i = 0; i < n; i++, (*total)++)
            if (*total < len - 1)
                out[*total] = chunk[i];
    }
}

/* Run one case: its exit status (-1 if it had to be killed) and what it wrote
 * to stdout, the first len - 1 bytes in out and the count in *total. Stdout
 * is drained while the child runs, so a child writing more than a pipe holds
 * is not stopped by the parent.
 */
static int se_run(const char *what, char *out, size_t len, size_t *total)
{
    int in[2], so[2], st = 0;
    posix_spawn_file_actions_t fa;
    char *argv[] = {"syscall_testing", "--stdio-exit-child", (char *) what, NULL};
    pid_t pid = -1;
    int rc;

    out[0] = '\0';
    *total = 0;
    if (pipe(in) != 0)
        return -2;
    if (pipe(so) != 0) {
        close(in[0]);
        close(in[1]);
        return -2;
    }
    (void) fcntl(in[1], F_SETFD, FD_CLOEXEC);
    (void) fcntl(so[0], F_SETFD, FD_CLOEXEC);
    (void) fcntl(so[0], F_SETFL, O_NONBLOCK);
    rc = posix_spawn_file_actions_init(&fa);
    if (!rc) {
        rc = posix_spawn_file_actions_adddup2(&fa, in[0], 0);
        if (!rc)
            rc = posix_spawn_file_actions_adddup2(&fa, so[1], 1);
        if (!rc)
            rc = posix_spawn(&pid, "/bin/syscall_testing", &fa, NULL, argv, NULL);
        posix_spawn_file_actions_destroy(&fa);
    }
    close(in[0]);
    close(so[1]);
    if (rc) {
        close(in[1]);
        close(so[0]);
        return -2;
    }

    rc = -1;
    for (int ms = 0; ms < SE_EXIT_MS; ms += 10) {
        pid_t w = waitpid(pid, &st, WNOHANG);

        if (w == pid) {
            rc = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
            break;
        }
        if (w == -1 && errno != EINTR) {
            rc = -2;
            break;
        }
        se_drain(so[0], out, len, total);
        sleep_ms(10);
    }
    if (rc < 0) {
        (void) kill(pid, SIGKILL);
        (void) waitpid(pid, &st, 0);
    }
    close(in[1]); /* only now: the reader in the child must stay blocked */
    se_drain(so[0], out, len, total); /* the child's end is closed: no wait */
    out[*total < len - 1 ? *total : len - 1] = '\0';
    close(so[0]);
    return rc;
}

void test_stdio_exit(void)
{
    char out[128], detail[200];
    size_t total;
    int rc;

    rc = se_run("setvbuf", out, sizeof out, &total);
    snprintf(detail, sizeof detail, "status %d, stdout \"%s\"", rc, out);
    check(rc == 0 && !strcmp(out, "stdio-exit setvbuf\n"),
          "exit() flushes stdout buffered through setvbuf", detail);

    rc = se_run("held", out, sizeof out, &total);
    snprintf(detail, sizeof detail, "status %d (-1: still running after %d ms), stdout \"%s\"",
             rc, SE_EXIT_MS, out);
    check(rc == 0, "exit() returns while another thread is blocked in fgets(stdin)",
          detail);
    check(!strcmp(out, "stdio-exit held\n"),
          "exit() flushes stdout while another thread holds stdin", detail);

    rc = se_run("writer", out, sizeof out, &total);
    snprintf(detail, sizeof detail, "status %d, stdout \"%s\"", rc, out);
    check(rc == 0 && !strcmp(out, "stdio-exit writer\n"),
          "exit() waits for and flushes a held stream with pending output", detail);

    rc = se_run("bulk", out, sizeof out, &total);
    snprintf(detail, sizeof detail, "status %d, %zu of %d bytes", rc, total, SE_BULK);
    check(rc == 0 && total == SE_BULK,
          "exit() waits for a thread in the middle of writing a held stream", detail);
}
