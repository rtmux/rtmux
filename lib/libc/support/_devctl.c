/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <devctl.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>

int _devctl(int fd,
            unsigned devctl_cmd,
            void *data_ptr,
            size_t nbytes,
            unsigned flags)
{
    io_devctl_t msg;
    iov_t iov[4];
    int (*send)(int, const iov_t *, int, const iov_t *, int);

    msg.i.type = _IO_DEVCTL;
    msg.i.combine_len = sizeof msg.i;
    msg.i.devctl_cmd = devctl_cmd;
    msg.i.nbytes = (int32_t) nbytes;
    msg.i.zero = 0;

    SETIOV(iov + 0, &msg.i, sizeof msg.i);
    SETIOV(iov + 1, data_ptr,
           ((unsigned) devctl_cmd & _POSIX_DEVDIR_TO) ? nbytes : 0);

    SETIOV(iov + 2, &msg.o, sizeof msg.o);
    SETIOV(iov + 3, data_ptr,
           ((unsigned) devctl_cmd & _POSIX_DEVDIR_FROM) ? nbytes : 0);

    if (flags & _DEVCTL_FLAG_NOCANCEL)
        send = msg_sendvnc;
    else
        send = msg_sendv;

    if (send(fd, iov, 2, iov + 2, 2) == -1) {
        if ((flags & _DEVCTL_FLAG_NOTTY) && errno == ENOSYS)
            errno = ENOTTY;
        return -1;
    }
    return (flags & _DEVCTL_FLAG_NORETVAL) ? 0 : msg.o.ret_val;
}
