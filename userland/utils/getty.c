/*
 * toolbox getty - login banner/name reader applet.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "toolbox.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define GETTY_LOGIN_PATH "/bin/login"
#define GETTY_NAME_MAX 32

static void getty_banner(void)
{
    fputs("RTMux\n\n", stdout);
    fflush(stdout);
}

static int getty_read_name(char *buf, size_t bufsz)
{
    size_t len;

    if (!fgets(buf, (int) bufsz, stdin))
        return -1;
    len = strlen(buf);
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';
    return (int) len;
}

static int getty_name_is_valid(const char *name)
{
    if (name[0] == '\0' || name[0] == '-')
        return 0;
    for (const char *p = name; *p != '\0'; p++) {
        if (!isalnum((unsigned char) *p) && *p != '_' && *p != '.' && *p != '-')
            return 0;
    }
    return 1;
}

static int getty_open_tty(const char *path)
{
    int fd;

    if (!path)
        return STDIN_FILENO;

    /* Become our own session leader before opening the tty so the kernel can
     * make it controlling.
     */
    setsid();
    /* ipc-send: utils -> roottask-fsio,roottask-core,romfs,lkl-fsd,lkl-netd,dev-nvme,dev-serial,pipe,logger-ring */
    fd = open(path, O_RDWR);
    if (fd < 0) {
        perror(path);
        return -1;
    }
    if (fd != STDIN_FILENO)
        dup2(fd, STDIN_FILENO);
    if (fd != STDOUT_FILENO)
        dup2(fd, STDOUT_FILENO);
    if (fd != STDERR_FILENO)
        dup2(fd, STDERR_FILENO);
    if (fd > STDERR_FILENO)
        close(fd);
    /* open() already made the terminal ours when it could (a session leader's
     * first terminal), so EPERM from a repeat bind is expected; any other
     * failure means ^C/^Z will not reach the jobs, so say so.
     */
    if (tcsetsid(STDIN_FILENO, getpgrp()) == -1 && errno != EPERM)
        fprintf(stderr, "getty: %s: cannot become controlling terminal: %s\n",
                path, strerror(errno));
    return STDIN_FILENO;
}

int toolbox_getty_main(int argc, char **argv)
{
    int fd;
    char name[GETTY_NAME_MAX + 1];
    char *spawn_argv[3];
    pid_t pid;
    int status;

    if (argc > 2) {
        fprintf(stderr, "usage: getty [TTY]\n");
        return 1;
    }
    fd = getty_open_tty(argc == 2 ? argv[1] : NULL);
    if (fd < 0)
        return 1;

    /* ^C and ^\ typed at the prompt signal the terminal's foreground group,
     * which is this one until a job takes over; getty must outlive them.
     */
    (void) signal(SIGINT, SIG_IGN);
    (void) signal(SIGQUIT, SIG_IGN);
    (void) signal(SIGTTOU, SIG_IGN);

    for (;;) {
        name[0] = '\0';
        getty_banner();

        for (;;) {
            int n;

            fputs("login: ", stdout);
            fflush(stdout);
            n = getty_read_name(name, sizeof(name));
            if (n < 0)
                break;
            if (n == 0)
                continue;
            if (!getty_name_is_valid(name)) {
                fputs("invalid login name\n", stdout);
                continue;
            }
            break;
        }
        if (name[0] == '\0')
            continue;

        spawn_argv[0] = (char *) "login";
        spawn_argv[1] = name;
        spawn_argv[2] = NULL;
        if (posix_spawn(&pid, GETTY_LOGIN_PATH, NULL, NULL, spawn_argv, NULL) !=
            0) {
            perror(GETTY_LOGIN_PATH);
            sleep(1);
            continue;
        }
        if (waitpid(pid, &status, 0) < 0) {
            perror("waitpid");
            sleep(1);
        }
        (void) status;
        /* A job-control shell hands the terminal back when it exits; make sure
         * of it, or the next login prompt would read as a background process.
         */
        (void) tcsetpgrp(fd, getpgrp());
    }
}
