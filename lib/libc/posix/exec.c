/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/procmsg.h>

extern char **environ;

static int exec_spawn(const char *path,
                      char *const argv[],
                      char *const envp[],
                      int search)
{
    struct inheritance inherit = {
        .flags = SPAWN_EXEC,
    };
    int fd_map[] = {
        [0] = STDIN_FILENO,
        [1] = STDOUT_FILENO,
        [2] = STDERR_FILENO,
    };
    pid_t pid;

    pid = search ? rtmux_spawnp(path, 3, fd_map, &inherit, argv, envp)
                 : rtmux_spawn(path, 3, fd_map, &inherit, argv, envp);
    if (pid == -1)
        return -1;
    errno = EIO;
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return exec_spawn(path, argv, envp, 0);
}

int execv(const char *path, char *const argv[])
{
    return execve(path, argv, environ);
}

int execvp(const char *file, char *const argv[])
{
    return exec_spawn(file, argv, environ, 1);
}

static int count_args(const char *arg0, va_list ap)
{
    int argc = 1;

    while (va_arg(ap, const char *))
        argc++;
    return argc;
}

static int execv_from_args(const char *path,
                           const char *arg0,
                           va_list ap,
                           char *const envp[],
                           int search)
{
    va_list count_ap;
    char **argv;
    int argc;

    va_copy(count_ap, ap);
    argc = count_args(arg0, count_ap);
    va_end(count_ap);

    argv = malloc((size_t) (argc + 1) * sizeof(*argv));
    if (!argv) {
        errno = ENOMEM;
        return -1;
    }

    argv[0] = (char *) arg0;
    for (int i = 1; i < argc; i++)
        argv[i] = va_arg(ap, char *);
    argv[argc] = NULL;

    int ret = exec_spawn(path, argv, envp, search);
    free(argv);
    return ret;
}

int execl(const char *path, const char *arg0, ...)
{
    va_list ap;
    int ret;

    va_start(ap, arg0);
    ret = execv_from_args(path, arg0, ap, environ, 0);
    va_end(ap);
    return ret;
}

int execlp(const char *file, const char *arg0, ...)
{
    va_list ap;
    int ret;

    va_start(ap, arg0);
    ret = execv_from_args(file, arg0, ap, environ, 1);
    va_end(ap);
    return ret;
}

int execle(const char *path, const char *arg0, ...)
{
    va_list ap;
    va_list scan;
    char *const *envp;
    int ret;

    va_start(ap, arg0);
    va_copy(scan, ap);
    while (va_arg(scan, char *))
        ;
    envp = va_arg(scan, char *const *);
    va_end(scan);
    ret = execv_from_args(path, arg0, ap, (char *const *) envp, 0);
    va_end(ap);
    return ret;
}
