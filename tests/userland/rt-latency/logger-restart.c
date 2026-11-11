/*
 * logger-restart - user-mode restart contract probe for the logger service.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/slog.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define LOGGER_PATH "/sbin/logger"
#define WATCH_PATH "/dev/slog"
#define POLL_MS 100
#define TIMEOUT_MS 10000
#define POLL_MAX 100

static void nap_ms(long ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (ms % 1000) * 1000000,
    };

    (void) nanosleep(&ts, NULL);
}

static int wait_present(const char *path)
{
    for (long waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
        int fd = open(path, O_RDONLY | O_NONBLOCK);

        if (fd != -1) {
            close(fd);
            return 0;
        }
        nap_ms(POLL_MS);
    }
    return -1;
}

static int wait_absent(const char *path)
{
    for (long waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
        if (access(path, F_OK) == -1 && errno == ENOENT)
            return 0;
        nap_ms(POLL_MS);
    }
    return -1;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return slash ? slash + 1 : path;
}

static int read_proc_name(pid_t pid, char *buf, size_t len)
{
    char path[64];
    ssize_t n;
    int fd;

    snprintf(path, sizeof(path), "/proc/%d/name", (int) pid);
    fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;
    n = read(fd, buf, len - 1);
    close(fd);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

static pid_t find_logger(void)
{
    DIR *dp = opendir("/proc");
    struct dirent *de;

    if (!dp)
        return -1;
    while ((de = readdir(dp))) {
        char name[PATH_MAX];
        char *end;
        long pid;

        if (!isdigit((unsigned char) de->d_name[0]))
            continue;
        pid = strtol(de->d_name, &end, 10);
        if (*end != '\0' || pid <= 0)
            continue;
        if (read_proc_name((pid_t) pid, name, sizeof(name)) == 0 &&
            strcmp(base_name(name), "logger") == 0) {
            closedir(dp);
            return (pid_t) pid;
        }
    }
    closedir(dp);
    return -1;
}

static int wait_logger_pid(pid_t pid)
{
    for (long waited = 0; waited < TIMEOUT_MS; waited += POLL_MS) {
        char name[PATH_MAX];

        if (read_proc_name(pid, name, sizeof(name)) == 0 &&
            strcmp(base_name(name), "logger") == 0)
            return 0;
        nap_ms(POLL_MS);
    }
    return -1;
}

static int start_logger(pid_t *pid)
{
    char *argv[] = {
        (char *) LOGGER_PATH,
        (char *) "-C",
        NULL,
    };
    int rc = posix_spawn(pid, LOGGER_PATH, NULL, NULL, argv, environ);

    if (rc != EOK) {
        fprintf(stderr, "logger-restart: spawn: %s\n", strerror(rc));
        return -1;
    }
    if (wait_present(WATCH_PATH) != 0) {
        fprintf(stderr, "logger-restart: %s did not publish\n", WATCH_PATH);
        return -1;
    }
    if (wait_logger_pid(*pid) != 0) {
        fprintf(stderr, "logger-restart: second logger did not stay alive\n");
        return -1;
    }
    return 0;
}

int main(void)
{
    pid_t first = -1;
    pid_t second;
    int record[_SLOG_HDRINTS];
    int fd;

    if (wait_present(WATCH_PATH) != 0) {
        fprintf(stderr, "logger-restart: %s did not publish\n", WATCH_PATH);
        return 1;
    }
    first = find_logger();
    if (first <= 0) {
        fprintf(stderr, "logger-restart: cannot find first logger pid\n");
        return 1;
    }

    fd = open(WATCH_PATH, O_RDWR | O_NONBLOCK);
    if (fd == -1) {
        fprintf(stderr, "logger-restart: open old handle: %s\n",
                strerror(errno));
        return 1;
    }

    memset(record, 0, sizeof(record));
    if (write(fd, record, sizeof(record)) != (ssize_t) sizeof(record)) {
        fprintf(stderr, "logger-restart: crash write: %s\n", strerror(errno));
        close(fd);
        return 1;
    }

    for (int i = 0; i < POLL_MAX; i++) {
        errno = 0;
        if (read(fd, record, sizeof(record)) == -1 && errno == EBADF)
            break;
        nap_ms(POLL_MS);
    }
    if (errno != EBADF) {
        fprintf(stderr, "logger-restart: old handle did not fail EBADF\n");
        close(fd);
        return 1;
    }
    close(fd);

    if (unlink(WATCH_PATH) == -1 && errno != ENOENT && errno != ENXIO) {
        fprintf(stderr, "logger-restart: unlink stale %s: %s\n", WATCH_PATH,
                strerror(errno));
        return 1;
    }
    if (wait_absent(WATCH_PATH) != 0) {
        fprintf(stderr, "logger-restart: %s did not vanish\n", WATCH_PATH);
        return 1;
    }

    if (start_logger(&second) != 0)
        return 1;
    if (second == first) {
        fprintf(stderr, "logger-restart: pid was reused without generation\n");
        return 1;
    }

    printf("logger-restart: PASS first=%d second=%d\n", (int) first,
           (int) second);
    return 0;
}
