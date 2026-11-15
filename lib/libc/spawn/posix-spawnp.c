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

static int parse_envvar_str(char *str, const char *token, char ***value);

/* posix_spawnp — like posix_spawn(), except that <file> may be a bare name; in
 * that case the $PATH of the calling process is searched for the executable,
 * and posix_spawn() is then invoked with the resolved absolute path.
 *
 * Errors:
 *   ENOENT  - <file> not found in any $PATH component (or $PATH unset).
 *   ENOMEM  - couldn't strdup($PATH) for tokenisation.
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

    int num_paths;
    int r = -1;
    int i;
    char *envvar_str;
    const char *path_env;
    char **env_path;
    char path[PATH_MAX + 1];

    /* Copy PATH under the environment lock, so setenv cannot change it or free
     * the vector while it is read.
     */
    __env_lock();
    path_env = getenv("PATH");
    envvar_str = path_env ? strdup(path_env) : NULL;
    __env_unlock();
    if (!path_env)
        return ENOENT;
    if (!envvar_str)
        return ENOMEM;

    if ((num_paths = parse_envvar_str(envvar_str, ":", &env_path)) <= 0) {
        free(envvar_str);
        return ENOENT;
    }

    for (i = 0; i < num_paths; i++) {
        struct stat dummy;

        /* Resolve a relative $PATH entry against the caller's cwd, as the
         * stat() below does, so posix_spawn() cannot resolve it against the
         * directory chdir file actions leave and run a different file.
         */
        if (env_path[i][0] != '/') {
            char *b;
            int trys = 5;

            while (--trys >= 0 && !(b = getcwd(path, sizeof(path))) &&
                   (r = errno) == EAGAIN)
                ;
            if (!b) {
                if (r == EAGAIN)
                    break; /* too many retries — give up */
                continue;  /* try next $PATH entry */
            }
            size_t len = strlen(path);
            size_t elen = strlen(env_path[i]);
            int n;

            while (elen > 0 && env_path[i][elen - 1] == '/')
                elen--;
            /* "" and "." name the cwd itself; "./" was the old special case */
            if (elen == 1 && env_path[i][0] == '.')
                elen = 0;
            n = snprintf(path + len, sizeof(path) - len, "%s%.*s%s%s",
                         len > 1 ? "/" : "", (int) elen, env_path[i],
                         elen ? "/" : "", file);

            if (n < 0 || (size_t) n >= sizeof(path) - len) {
                r = ENAMETOOLONG;
                break;
            }
        } else {
            int n = snprintf(path, sizeof(path), "%s/%s", env_path[i], file);

            if (n < 0 || (size_t) n >= sizeof(path)) {
                r = ENAMETOOLONG;
                break;
            }
        }

        if (stat(path, &dummy) == 0) {
            r = posix_spawn(pid, path, file_actions, attrp, argv, envp);

            /* POSIX: continue the search past EACCES, but stop on any other
             * error (or success).
             */
            if (r != EACCES)
                break;
        } else {
            r = errno;
        }
    }

    free(env_path);
    free(envvar_str);
    return r != -1 ? r : ENOENT;
}

/* parse_envvar_str — split <str> at <token> in place, returning the count of
 * tokens and a malloc'd array of pointers in *value. Caller frees *value on a
 * positive return.
 */
static int parse_envvar_str(char *str, const char *token, char ***value)
{
    int n = 0;
    char *saveptr;
    char *s = strtok_r(str, token, &saveptr);

    *value = NULL;
    while (s) {
        char **tmp = realloc(*value, (size_t) (n + 1) * sizeof(*tmp));
        if (!tmp) {
            free(*value);
            *value = NULL;
            return -1;
        }
        *value = tmp;
        (*value)[n++] = s;
        s = strtok_r(NULL, token, &saveptr);
    }
    return n;
}
