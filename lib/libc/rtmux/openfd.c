/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <sys/iomsg.h>
#include <sys/node.h>

#include <posix.h>

static int sopenfd_from_info(pid_t source_pid,
                             int source_fd,
                             const struct _server_info *info,
                             int oflag,
                             int sflag,
                             int xtype)
{
    int fd2;
    io_openfd_t msg;

    if ((fd2 = connect_attach(info->nd, info->pid, info->chid, 0,
                              RTMUX_FLG_COF_CLOEXEC)) == -1) {
        return -1;
    }

    memset(&msg.i, 0x00, sizeof msg.i);
    msg.i.type = _IO_OPENFD;
    msg.i.combine_len = sizeof msg.i;
    msg.i.ioflag = (uint32_t) ((oflag & ~(O_ACCMODE | O_CLOEXEC)) |
                               ((oflag + 1) & O_ACCMODE));
    msg.i.sflag = (uint16_t) sflag;
    msg.i.xtype = (uint16_t) xtype;
    msg.i.info.nd = RTMUX_LOCAL_NODE;
    msg.i.info.pid = source_pid;
    msg.i.info.chid = info->chid;
    msg.i.info.scoid = info->scoid;
    msg.i.info.coid = source_fd;

    if (msg_send(fd2, &msg.i, sizeof msg.i, 0, 0) == -1) {
        __connect_detach((uint64_t) fd2);
        return -1;
    }

    if (!(oflag & O_CLOEXEC))
        connect_flags_r(0, (uint64_t) fd2, FD_CLOEXEC, 0);

    return fd2;
}

int __rtmux_openfd_typed(int fd, int oflag, int sflag, int xtype)
{
    struct _server_info info;

    if (fd == -1 || connect_server_info(0, (uint64_t) fd, &info) != fd) {
        errno = EBADF;
        return -1;
    }

    return sopenfd_from_info(getpid(), fd, &info, oflag, sflag, xtype);
}

static const char *parse_procfd_pid(const char *path, pid_t *pid)
{
    char *end;
    unsigned long v;

    if (strncmp(path, "/proc/", 6) != 0)
        return NULL;
    path += 6;
    if (strncmp(path, "self", 4) == 0) {
        path += 4;
        *pid = getpid();
    } else {
        if (*path < '0' || *path > '9')
            return NULL;
        errno = 0;
        v = strtoul(path, &end, 10);
        if (errno == ERANGE || v == 0 || v > (unsigned long) INT_MAX)
            return NULL;
        *pid = (pid_t) v;
        path = end;
    }
    if (strncmp(path, "/fd/", 4) != 0)
        return NULL;
    return path + 4;
}

int __rtmux_procfd_open(const char *path, int oflag, int sflag, int *matched)
{
    struct _server_info info;
    const char *fdstr;
    char *end;
    pid_t pid;
    long fd;

    if (matched)
        *matched = 0;
    if (!path || !matched)
        return -1;

    fdstr = parse_procfd_pid(path, &pid);
    if (!fdstr)
        return -1;

    /* Another process's descriptor is reopened by roottask, which mints the key
     * an I/O service demands; libc has none to attach.
     */
    if (pid != getpid())
        return -1;
    *matched = 1;

    if (*fdstr < '0' || *fdstr > '9') {
        errno = ENOENT;
        return -1;
    }
    errno = 0;
    fd = strtol(fdstr, &end, 10);
    if (errno == ERANGE || *end != '\0' || fd < 0 || fd > INT_MAX) {
        errno = ENOENT;
        return -1;
    }
    if ((oflag & (O_CREAT | O_EXCL)) == (O_CREAT | O_EXCL)) {
        errno = EEXIST;
        return -1;
    }
    if ((oflag & O_TRUNC) != 0) {
        errno = EINVAL;
        return -1;
    }

    if (connect_server_info(pid, (uint64_t) (int) fd, &info) != (int) fd) {
        errno = EBADF;
        return -1;
    }
    return sopenfd_from_info(pid, (int) fd, &info, oflag, sflag,
                             _IO_OPENFD_NONE);
}
