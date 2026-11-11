/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rtmux-shim.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>

#include <sys/iomsg.h>
#include <sys/rtmux-core.h>

/* AT_FDCWD: "current working directory" sentinel for *at() functions. RTMux
 * does not define this yet; use the POSIX standard value.
 */
#ifndef AT_FDCWD
#define AT_FDCWD (-100)
#endif

extern int _vopen(const char *path, int oflag, int sflag, va_list ap);

int rtmux_ldso_open(const char *path, int flags, ...)
{
    va_list ap;
    int fd;

    va_start(ap, flags);
    fd = _vopen(path, flags, 0, ap);
    va_end(ap);
    return fd;
}

int rtmux_ldso_close(int fd)
{
    io_close_t msg;
    int ret, err;

    msg.i.type = _IO_CLOSE;
    msg.i.combine_len = sizeof msg.i;
    if ((ret = msg_send(fd, &msg.i, sizeof msg.i, 0, 0)) != -1 ||
        errno != EINTR) {
        if ((err = connect_detach_r((uint64_t) fd)) == EOK)
            return ret;
        if (ret != -1)
            errno = err;
    }
    return -1;
}

ssize_t rtmux_ldso_read(int fd, void *buf, size_t nbytes)
{
    io_read_t msg;

    msg.i.type = _IO_READ;
    msg.i.combine_len = sizeof msg.i;
    msg.i.nbytes = nbytes;
    msg.i.xtype = _IO_XTYPE_NONE;
    msg.i.zero = 0;
    return msg_send(fd, &msg.i, sizeof msg.i, buf, nbytes);
}

ssize_t rtmux_ldso_write(int fd, const void *buf, size_t nbytes)
{
    io_write_t msg;
    iov_t iov[2];

    msg.i.type = _IO_WRITE;
    msg.i.combine_len = sizeof msg.i;
    msg.i.xtype = _IO_XTYPE_NONE;
    msg.i.nbytes = (int32_t) nbytes;
    msg.i.zero = 0;
    SETIOV(iov + 0, &msg.i, sizeof msg.i);
    SETIOV(iov + 1, buf, nbytes);
    return msg_sendv(fd, iov, 2, 0, 0);
}

ssize_t rtmux_ldso_pread(int fd, void *buf, size_t nbytes, off_t offset)
{
    struct {
        io_read_t rd;
        struct _xtype_offset xoff;
    } msg;

    msg.rd.i.type = _IO_READ;
    msg.rd.i.combine_len = sizeof msg.rd.i + sizeof msg.xoff;
    msg.rd.i.nbytes = nbytes;
    msg.rd.i.xtype = _IO_XTYPE_OFFSET;
    msg.rd.i.zero = 0;
    msg.xoff.offset = offset;
    return msg_send(fd, &msg, sizeof msg, buf, nbytes);
}

int rtmux_ldso_fstat(int fd, struct stat *sb)
{
    io_stat_t msg;

    msg.i.type = _IO_STAT;
    msg.i.combine_len = sizeof msg.i;
    msg.i.zero = 0;
    if (msg_sendnc(fd, &msg.i, sizeof msg.i, sb, sizeof *sb) == -1)
        return -1;
    if (sb->st_size < 0) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

int rtmux_ldso_fcntl(int fd, int cmd, ...)
{
    (void) fd;
    switch (cmd) {
    case F_GETFD:
    case F_SETFD:
        return 0;
    default:
        errno = EINVAL;
        return -1;
    }
}

int rtmux_ldso_openat(int dirfd, const char *path, int flags, ...)
{
    if (dirfd == AT_FDCWD) {
        va_list ap;
        int fd;

        va_start(ap, flags);
        fd = _vopen(path, flags, 0, ap);
        va_end(ap);
        return fd;
    }
    errno = ENOSYS;
    return -1;
}

int rtmux_ldso_fstatat(int dirfd, const char *path, struct stat *sb, int flags)
{
    (void) flags;
    if (dirfd == AT_FDCWD) {
        int fd = rtmux_ldso_open(path, O_RDONLY | O_CLOEXEC);
        if (fd == -1)
            return -1;
        int ret = rtmux_ldso_fstat(fd, sb);
        rtmux_ldso_close(fd);
        return ret;
    }
    errno = ENOSYS;
    return -1;
}

int rtmux_ldso_getcwd(char *buf, size_t size)
{
    if (size < 2) {
        errno = ERANGE;
        return -1;
    }
    buf[0] = '/';
    buf[1] = '\0';
    return 0;
}
