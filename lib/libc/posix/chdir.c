/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <alloca.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/pathmsg.h>
#include "connect.h"
#include <posix.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

/* Ask proc to make `path` (absolute, relative to the process root) the cwd of
 * the sending process. The spawn loader also uses this to set a child's cwd.
 */
int __chdir_send(const char *path)
{
    struct _io_connect msg = {
        .type = _PATH_CHDIR,
        .path_len = (uint32_t) (strlen(path) + 1),
    };
    iov_t iov[2];

    SETIOV(iov + 0, &msg, offsetof(struct _io_connect, path));
    SETIOV(iov + 1, path, msg.path_len);
    if (msg_sendv(PATH_COID, iov, 2, 0, 0) == -1)
        return -1;
    return 0;
}

/* Resolve `path` as chdir() would, without changing the working directory: the
 * target must exist, be a directory, and grant the caller search permission. On
 * success the canonical path relative to the process root, the form _PATH_CHDIR
 * takes, is written to `out`.
 *
 * Returns 0, or -1 with errno set. posix_spawn() uses this to check addchdir
 * and addfchdir targets in the parent.
 */
int __chdir_resolve(const char *path, char *out, size_t outsz)
{
    struct _connect_ctrl ctrl;
    struct _io_connect msg;
    int fd;
    int euid;
    struct _io_stat s;
    struct stat r;
    size_t len;

    if (outsz < 2 || outsz > INT_MAX) {
        errno = EINVAL;
        return -1;
    }
    /* The root of the path namespace has no object to stat, so the lookup below
     * answers EINVAL for it and there is no mode to check: it is always
     * searchable.
     */
    if (path && !strcmp(path, "/")) {
        memcpy(out, "/", 2);
        return 0;
    }

    /* Resolve straight into out; the chroot prefix is stripped in place. */
    memset(&ctrl, 0x00, sizeof ctrl);
    ctrl.base = RTMUX_SIDE_CHANNEL;
    ctrl.path = out;
    ctrl.pathsize = (int) outsz;
    ctrl.send = msg_sendvnc;
    ctrl.msg = &msg;
    ctrl.extra = &s;

    s.type = _IO_STAT;
    s.combine_len = sizeof s;
    s.zero = 0;

    memset(&msg, 0x00, sizeof msg);
    msg.ioflag = O_NOCTTY;
    msg.subtype = _IO_CONNECT_COMBINE_CLOSE;
    msg.extra_len = sizeof s;

    if ((fd = _connect_ctrl(&ctrl, path, sizeof r, &r)) == -1) {
        return -1;
    }
    __connect_detach((uint64_t) fd);

    if (out[0] == '\0') {
        errno = ENAMETOOLONG;
        return -1;
    }

    if (!S_ISDIR(r.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    /* Need to permission check the last component to verify that we have
     * execute permission on this last component
     */
    if ((euid = geteuid()) == 0) {
        ; /* OK */
    } else if (euid == r.st_uid) {
        if (!(r.st_mode & S_IXUSR)) {
            errno = EACCES;
            return -1;
        }
    } else {
        gid_t *grouplist;
        int glsize;

        glsize = getgroups(0, NULL) + 1;
        if (!(grouplist = alloca((size_t) glsize * sizeof(gid_t)))) {
            errno = ENOMEM;
            return -1;
        }

        grouplist[0] = getegid();
        glsize = getgroups(glsize - 1, &grouplist[1]) + 1;
        while (--glsize >= 0) {
            if (r.st_gid == grouplist[glsize]) {
                break;
            }
        }

        /* If no matching group id's then we are an 'other' */
        if (glsize >= 0 && !(r.st_mode & S_IXGRP)) {
            errno = EACCES;
            return -1;
        } else if (glsize < 0 && !(r.st_mode & S_IXOTH)) {
            errno = EACCES;
            return -1;
        }
    }

    /* proc resolves the path against the process root: drop the chroot part. */
    len = strlen(out);
    if (ctrl.chroot_len >= len)
        memcpy(out, "/", 2); /* we are in the chroot-ed directory */
    else
        memmove(out, out + ctrl.chroot_len, len - ctrl.chroot_len + 1);
    return 0;
}

int chdir(const char *path)
{
    char resolved[PATH_MAX + 1];

    if (__chdir_resolve(path, resolved, sizeof resolved) != 0 ||
        __chdir_send(resolved) != 0)
        return -1;
    __atfd_chdir(resolved);
    return 0;
}
