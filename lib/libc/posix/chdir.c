/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <alloca.h>
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

static int chdir_send(const char *path)
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

int chdir(const char *path)
{
    struct _connect_ctrl ctrl;
    struct _io_connect msg;
    int fd;
    int euid;
    int ret;
    /* Should use fpathconf(path, _PC_PATH_MAX) */
    char buffer[PATH_MAX + 1];
    struct _io_stat s;
    struct stat r;

    if (path && !strcmp(path, "/")) {
        int rc = chdir_send(path);

        if (rc == 0)
            __atfd_chdir("/");
        return rc;
    }

    memset(&ctrl, 0x00, sizeof ctrl);
    ctrl.base = RTMUX_SIDE_CHANNEL;
    ctrl.path = buffer;
    ctrl.pathsize = sizeof buffer;
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

    if (buffer[0] == '\0') {
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

    /* Pass the fully resolved path to proc. */
    path = buffer;
    if (ctrl.chroot_len >= strlen(path)) {
        /* we are in the chroot-ed directory */
        path = "/";
    } else {
        path += ctrl.chroot_len;
    }
    ret = chdir_send(path);
    if (ret == 0)
        __atfd_chdir(path);
    return ret;
}
