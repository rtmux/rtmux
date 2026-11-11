/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <devctl.h>
#include <devctl/common.h>
#include <sys/iomsg.h>
#include <sys/node.h>
#include <sys/rtmux-core.h>

#include <posix.h>

/* Duplicate `fd` onto the lowest free descriptor >= `minfd`, setting the new
 * descriptor's close-on-exec flag to `fdflags` (0 for F_DUPFD, FD_CLOEXEC for
 * F_DUPFD_CLOEXEC).
 */
static int dup_fd(int fd, int minfd, unsigned fdflags)
{
    struct _server_info info;
    io_dup_t dup;
    int fd2;

    if (fd == -1 || ((unsigned) fd & RTMUX_SIDE_CHANNEL) ||
        connect_server_info(0, (uint64_t) fd, &info) != fd) {
        errno = EBADF;
        return -1;
    }
    if (minfd < 0 || ((unsigned) minfd & RTMUX_SIDE_CHANNEL)) {
        errno = EINVAL;
        return -1;
    }
    if ((fd2 = connect_attach(info.nd, info.pid, info.chid, (unsigned) minfd,
                              RTMUX_FLG_COF_CLOEXEC)) == -1) {
        return -1;
    }
    dup.i.type = _IO_DUP;
    dup.i.combine_len = sizeof dup;
    dup.i.info.nd = RTMUX_LOCAL_NODE;
    dup.i.info.pid = getpid();
    dup.i.info.chid = info.chid;
    dup.i.info.scoid = info.scoid;
    dup.i.info.coid = fd;
    if (msg_sendnc(fd2, &dup.i, sizeof dup.i, 0, 0) == -1) {
        connect_detach_r((uint64_t) fd2);
        return -1;
    }
    connect_flags_r(0, (uint64_t) fd2, FD_CLOEXEC, fdflags);
    __atfd_dup(fd, fd2);
    return fd2;
}

static int vfcntl(int fd, int cmd, va_list ap)
{
    union {
        io_space_t space;
        io_lock_t lock;
    } msg;
    iov_t iov[4];
    int arg;

    switch (cmd) {
    case F_DUPFD:
        return dup_fd(fd, va_arg(ap, int), 0);

    case F_DUPFD_CLOEXEC:
        return dup_fd(fd, va_arg(ap, int), FD_CLOEXEC);

    case F_GETFD:
        return connect_flags(0, (uint64_t) fd, 0, 0);

    case F_SETFD:
        /* Only the close-on-exec bit: an all-ones mask would also select
         * RTMUX_FLG_COF_REVOKE_SEND and permanently revoke the fd.
         */
        return connect_flags(0, (uint64_t) fd, FD_CLOEXEC,
                             (unsigned) va_arg(ap, int) & FD_CLOEXEC);

    case F_GETFL:
        if (_devctl(fd, DEVCTL_GET_FLAGS, &arg, sizeof arg, 0) == -1)
            return -1;
        return arg & (O_ACCMODE | O_SETFLAG);

    case F_SETFL:
        arg = va_arg(ap, int) & O_SETFLAG;
        return _devctl(fd, DEVCTL_SET_FLAGS, &arg, sizeof arg,
                       _DEVCTL_FLAG_NORETVAL);

    case F_ALLOCSP: {
        flock_t *area = va_arg(ap, flock_t *);
        msg.space.i.start = (uint64_t) area->l_start;
        msg.space.i.len = (uint64_t) area->l_len;
        msg.space.i.whence = area->l_whence;
        msg.space.i.type = _IO_SPACE;
        msg.space.i.combine_len = sizeof msg.space.i;
        msg.space.i.subtype = (uint16_t) cmd;
        return msg_send(fd, &msg.space.i, sizeof msg.space.i, 0, 0);
    }

    case F_FREESP: {
        flock_t *area = va_arg(ap, flock_t *);
        msg.space.i.start = (uint64_t) area->l_start;
        msg.space.i.len = (uint64_t) area->l_len;
        msg.space.i.whence = area->l_whence;
        msg.space.i.type = _IO_SPACE;
        msg.space.i.combine_len = sizeof msg.space.i;
        msg.space.i.subtype = (uint16_t) cmd;
        return msg_send(fd, &msg.space.i, sizeof msg.space.i, 0, 0);
    }

    case F_GETLK:
    case F_SETLK:
    case F_SETLKW:
        msg.lock.i.type = _IO_LOCK;
        msg.lock.i.combine_len = sizeof msg.lock.i;
        msg.lock.i.subtype = (uint32_t) cmd;
        SETIOV(iov + 0, &msg.lock.i, sizeof msg.lock.i);
        SETIOV(iov + 1, va_arg(ap, flock_t *), sizeof(flock_t));
        iov[3] = iov[1];
        SETIOV(iov + 2, &msg.lock.o, sizeof msg.lock.o);
        return msg_sendv(fd, iov + 0, 2, iov + 2, (cmd == F_GETLK) ? 2 : 1);

    default:
        break;
    }

    errno = EINVAL;
    return -1;
}

int fcntl(int fd, int cmd, ...)
{
    va_list ap;
    int ret;

    va_start(ap, cmd);
    ret = vfcntl(fd, cmd, ap);
    va_end(ap);
    return ret;
}
