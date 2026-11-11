/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <stdlib.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/pathmsg.h>
#include "connect.h"
#include <posix.h>

#ifndef PATH_MAX
#define PATH_MAX 1024 /* Should use fpathconf(path, _PC_PATH_MAX) */
#endif

int chroot(const char *path)
{
    struct _connect_ctrl ctrl;
    struct _io_connect msg;
    int fd;
    char buffer[PATH_MAX + 1];
    struct _io_stat s;
    struct stat r;
    iov_t iov[2];

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

    memset(&msg, 0x00, sizeof msg);
    msg.type = _PATH_CHROOT;

    /* instead of "path", we pass in "buffer"; this way, if "path" happened to
     * be a symlink or network path, it is already resolved in buffer. This
     * solves PR#6563, 8941, 11302, 14837.
     */
    buffer[PATH_MAX] = 0;
    msg.path_len = (uint32_t) (strlen(buffer) + 1);
    SETIOV(iov + 0, &msg, offsetof(struct _io_connect, path));
    SETIOV(iov + 1, buffer, msg.path_len);
    if (msg_sendv(PATH_COID, iov, 2, 0, 0) == -1)
        return -1;

    /* The root moved: cached chroot-relative *at paths (cwd and tracked dirfds)
     * would be reinterpreted under the new root, so drop them all.
     */
    __atfd_chroot();
    return 0;
}
