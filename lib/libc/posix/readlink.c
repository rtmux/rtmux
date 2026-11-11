/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <minmax.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/iomsg.h>
#include <sys/stat.h>
#include <errno.h>

#include <posix.h>

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

ssize_t readlink(const char *path, char *buf, size_t bufsize)
{
    struct {
        struct _io_connect_link_reply r;
        char path[PATH_MAX + 1];
    } msg;
    int fd;
    ssize_t len;

    if ((fd = _connect(RTMUX_SIDE_CHANNEL, path, S_IFLNK, O_NOCTTY, 0,
                       _IO_CONNECT_READLINK, 0, 0, 0, 0, 0, 0, sizeof msg, &msg,
                       0)) == -1) {
        return -1;
    }
    __connect_detach((uint64_t) fd);

    if (msg.r.path_len == 0)
        msg.r.path_len = (uint16_t) strlen(msg.path);

    len = (ssize_t) min((size_t) msg.r.path_len, bufsize);
    memcpy(buf, msg.path, (size_t) len);

    if (msg.r.path_len < bufsize)
        buf[msg.r.path_len] = '\0';

    return len;
}

ssize_t readlinkat(int dirfd, const char *path, char *buf, size_t bufsize)
{
    char abuf[PATH_MAX + 1];
    const char *rpath = path;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (dirfd != AT_FDCWD && path[0] != '/') {
        if (__atfd_abspath(dirfd, path, abuf, sizeof abuf) != 0)
            return -1;
        rpath = abuf;
    }
    return readlink(rpath, buf, bufsize);
}
