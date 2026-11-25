/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Adapted for RTMux, 2026. Reformatted to RTMux K&R + 4-space style. */

#include <spawn.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>

#include "env-lock.h"

#include "exec-path-default.h"
#include <exec-path.h>

/* posix_spawnp — like posix_spawn(), except that <file> may be a bare name; in
 * that case the $PATH of the calling process is searched for the executable,
 * and posix_spawn() is then invoked with the resolved absolute path.
 *
 * Errors:
 *   ENOENT  - <file> not found in any $PATH component.
 *   ENOMEM  - couldn't copy the search path.
 *   any errno from posix_spawn() — see posix-spawn.c.
 */
int posix_spawnp(pid_t *restrict pid,
                 const char *restrict file,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *restrict attrp,
                 char *const argv[restrict],
                 char *const envp[restrict])
{
    if (!file || *file == '\0')
        return EINVAL;

    /* A path with a slash is not searched; posix_spawn() resolves a relative
     * one against the directory the chdir file actions leave.
     */
    if (strchr(file, '/'))
        return posix_spawn(pid, file, file_actions, attrp, argv, envp);

    __env_lock();
    const char *env = getenv("PATH");
    bool use_default = env == NULL;
    char *dirs = env ? strdup(env) : NULL;
    __env_unlock();
    if (use_default)
        dirs = exec_path_default();
    if (!dirs)
        return use_default ? errno : ENOMEM;

    const char *cursor = dirs;
    int saved = ENOENT;
    while (cursor) {
        char path[PATH_MAX + 1];
        struct stat st;
        int rc = exec_path_candidate(&cursor, file, path, sizeof path, true);
        if (!rc && stat(path, &st) < 0)
            rc = errno;
        if (!rc)
            rc = posix_spawn(pid, path, file_actions, attrp, argv, envp);
        if (!exec_path_continue(rc, &saved)) {
            saved = rc;
            break;
        }
    }
    free(dirs);
    return saved;
}
