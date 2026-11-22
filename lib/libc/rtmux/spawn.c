/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <pthread.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>
#include <sys/iomsg.h>

#include "env-lock.h"
#include "exec-path-default.h"

extern char **environ;

static pid_t spawn_common(const char *path,
                          int fd_count,
                          const int fd_map[],
                          const struct inheritance *inherit,
                          char *const *argv,
                          char *const *envp,
                          uint64_t *proc_cap)
{
    proc_spawn_t msg;
    iov_t iov[6];
    char *const *arg;
    char *src;
    char *search = NULL; /* PATH, copied under the environment lock */
    char *dst, *data;
    size_t nargv = 0, narge = 0;
    pid_t pid;
    int coid;

    msg.i.type = _PROC_SPAWN;
    msg.i.subtype = _PROC_SPAWN_START;

    /* Hold the environment lock while PATH, argv, and envp are measured and
     * copied: a concurrent setenv may edit environ, or rewrite in place a value
     * that any vector the caller built can point at. It is released before the
     * spawn IPC.
     */
    __env_lock();
    if (!envp)
        envp = environ;

    if (inherit) {
        msg.i.parms = *inherit;
        if (inherit->flags & SPAWN_SEARCH_PATH) {
            const char *path_env = getenv("PATH");

            if (path_env) {
                size_t len = strlen(path_env) + 1;

                /* The wire length is 16 bits; a longer PATH cannot be sent. */
                if (len > UINT16_MAX || !(search = malloc(len))) {
                    __env_unlock();
                    errno = len > UINT16_MAX ? E2BIG : ENOMEM;
                    return -1;
                }
                memcpy(search, path_env, len);
            }
        }
    } else {
        msg.i.parms.flags = 0;
    }

    msg.i.nbytes = 0;
    if (argv) {
        for (arg = argv; *arg; msg.i.nbytes += strlen(*arg++) + 1, nargv++)
            ;
    }
    if (envp) {
        for (arg = envp; *arg; msg.i.nbytes += strlen(*arg++) + 1, narge++)
            ;
    }
    /* nargv and narge are 16 bits on the wire; count before narrowing. */
    if (nargv > UINT16_MAX || narge > UINT16_MAX) {
        __env_unlock();
        free(search);
        errno = E2BIG;
        return -1;
    }
    msg.i.nargv = (uint16_t) nargv;
    msg.i.narge = (uint16_t) narge;

    msg.i.nfds = (uint16_t) fd_count;
    msg.i.cwdlen = 0;

    coid = ROOTTASK_COID;

    /* Heap, not alloca: the environment has no size bound, and alloca cannot
     * report failure. malloc may run under the environment lock (env-lock.h).
     */
    data = malloc(msg.i.nbytes ? msg.i.nbytes : 1);
    if (!data) {
        __env_unlock();
        free(search);
        errno = ENOMEM;
        return -1;
    }
    dst = data;
    SETIOV(iov + 5, data, 0);

    if (argv) {
        for (arg = argv; (src = *arg); arg++) {
            while ((*dst++ = *src++))
                ;
        }
    }

    if (envp) {
        for (arg = envp; (src = *arg); arg++) {
            while ((*dst++ = *src++))
                ;
        }
    }
    __env_unlock();

    if (inherit && (inherit->flags & SPAWN_SEARCH_PATH) && !search) {
        search = exec_path_default();
        if (!search || strlen(search) >= UINT16_MAX) {
            int err = search ? E2BIG : errno;
            free(search);
            free(data);
            errno = err;
            return -1;
        }
    }

    SETIOV(iov + 0, &msg.i, sizeof msg.i);
    SETIOV(iov + 1, fd_map, (size_t) fd_count * sizeof fd_map[0]);
    msg.i.searchlen = (uint16_t) (search ? strlen(search) + 1 : 0);
    SETIOV(iov + 2, search, msg.i.searchlen);
    msg.i.pathlen = (uint16_t) (strlen(path) + 1);
    SETIOV(iov + 3, path, msg.i.pathlen);
    SETIOV(iov + 4, data, msg.i.nbytes);

    if (proc_cap) {
        struct _io_native_hdr reply;
        iov_t riov;
        int reply_status;

        *proc_cap = 0;
        memset(&reply, 0, sizeof reply);
        SETIOV(&riov, &reply, sizeof reply);

        /* Only a client that opts in gets a native cap header parsed out of the
         * reply; scope the opt-in to exactly this request. If the opt-in is
         * refused, do not ask for a cap: the reply would arrive unparsed, with
         * the proc service's own handle in xfers[0].id.
         */
        if (thread_ctl(RTMUX_TCTL_NATIVE_REPLY, (void *) 1) == -1) {
            free(data);
            free(search);
            return -1;
        }
        msg.i.parms.flags |= SPAWN_PROC_CAP;
        reply_status = msg_sendvnc(coid, iov + 0, 6, &riov, 1);
        thread_ctl(RTMUX_TCTL_NATIVE_REPLY, (void *) 0);
        if (reply_status < 0) {
            pid = reply_status;
        } else if (reply_status > 0) {
            /* Fallback: the proc service declined to mint a cap and sent the
             * plain reply, pid in the status, no payload.
             */
            pid = reply_status;
        } else {
            /* Cap reply: status is EOK and the child pid rides the payload (a
             * positive status would be clamped when a payload is present); the
             * cap is an optional transfer, so nxfers may be 0 if the parent
             * table was full.
             */
            pid = (pid_t) reply.request_id;
            if (reply.nxfers >= 1)
                *proc_cap = reply.xfers[0].id;
        }
    } else {
        pid = msg_sendvnc(coid, iov + 0, 6, 0, 0);
    }

    free(data);
    free(search);
    return pid;
}

pid_t rtmux_spawn(const char *path,
                  int fd_count,
                  const int fd_map[],
                  const struct inheritance *inherit,
                  char *const *argv,
                  char *const *envp)
{
    return spawn_common(path, fd_count, fd_map, inherit, argv, envp, NULL);
}

pid_t rtmux_spawn_cap(const char *path,
                      int fd_count,
                      const int fd_map[],
                      const struct inheritance *inherit,
                      char *const *argv,
                      char *const *envp,
                      uint64_t *proc_cap)
{
    return spawn_common(path, fd_count, fd_map, inherit, argv, envp, proc_cap);
}
