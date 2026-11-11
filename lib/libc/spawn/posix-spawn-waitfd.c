/*
 * RTMux process-handle spawn helper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <sys/wait.h>

int rtmux_spawn_waitfd(pid_t *restrict pid,
                       int *restrict waitfd,
                       const char *restrict path,
                       const posix_spawn_file_actions_t *file_actions,
                       const posix_spawnattr_t *restrict attrp,
                       char *const argv[restrict],
                       char *const envp[restrict])
{
    char procpath[64];
    pid_t child;
    int fd;
    int r;
    int saved_errno;

    if (!pid || !waitfd)
        return EINVAL;
    *waitfd = -1;
    *pid = -1;

    r = posix_spawn(&child, path, file_actions, attrp, argv, envp);
    if (r != EOK)
        return r;

    (void) snprintf(procpath, sizeof(procpath), "/proc/%d/wait", (int) child);
    fd = open(procpath, O_RDONLY | O_CLOEXEC);
    if (fd == -1) {
        /* Open failed after spawn succeeded. Reap the child so the caller never
         * has to know about a process it cannot wait on. Without roottask
         * support for atomic spawn-plus-wait-handle, this is the only way to
         * keep the contract that *pid is valid iff we return EOK.
         */
        saved_errno = errno;
        (void) kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) == -1 && errno == EINTR)
            ;
        return saved_errno;
    }

    *pid = child;
    *waitfd = fd;
    return EOK;
}
