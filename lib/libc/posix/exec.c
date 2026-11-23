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

#include "signal-state.h"

/* A successful exec does not return: roottask loads the new image, destroys
 * every other thread of this process, and replies to this thread by starting
 * the new image in it, in the same process (same pid, same descriptors but the
 * close-on-exec ones). Only a failure comes back, with the process as it was,
 * and the request carries no descriptor map because none is inherited: the
 * process keeps its own.
 *
 * A NULL envp passes environ, which the spawn path then reads under the
 * environment lock.
 */
static int exec_spawn(const char *path,
                      char *const argv[],
                      char *const envp[],
                      int search)
{
    struct inheritance inherit = {
        .flags =
            SPAWN_EXEC | SPAWN_SETSIGMASK | SPAWN_SETSIGIGN | SPAWN_SETSIGPEND,
    };
    pid_t pid;
    int err;

    /* The new image keeps what is ignored, what is blocked and what is pending;
     * a caught signal goes back to its default, so only the sets are sent. No
     * signal is delivered to this image from here on, unless the exec fails.
     */
    __libc_signal_exec_begin(&inherit.sigignore, &inherit.sigmask,
                             &inherit.sigpending);
    pid = search ? rtmux_spawnp(path, 0, NULL, &inherit, argv, envp)
                 : rtmux_spawn(path, 0, NULL, &inherit, argv, envp);
    err = errno;
    __libc_signal_exec_end();
    errno = err;
    if (pid == -1)
        return -1;

    /* A reply that carries a pid is a spawn, not an exec: roottask never sends
     * one for SPAWN_EXEC.
     */
    errno = EIO;
    return -1;
}

int execve(const char *path, char *const argv[], char *const envp[])
{
    return exec_spawn(path, argv, envp, 0);
}

int execv(const char *path, char *const argv[])
{
    return exec_spawn(path, argv, NULL, 0);
}

int execvp(const char *file, char *const argv[])
{
    return exec_spawn(file, argv, NULL, 1);
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
    ret = execv_from_args(path, arg0, ap, NULL, 0);
    va_end(ap);
    return ret;
}

int execlp(const char *file, const char *arg0, ...)
{
    va_list ap;
    int ret;

    va_start(ap, arg0);
    ret = execv_from_args(file, arg0, ap, NULL, 1);
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
