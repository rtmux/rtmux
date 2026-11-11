/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <devctl.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>

int devctl(int fd,
           unsigned devctl_cmd,
           void *data_ptr,
           size_t nbytes,
           int *info_ptr)
{
    io_devctl_t msg;
    iov_t iov[4];
    int status;

    /* Stuff the message. */
    msg.i.type = _IO_DEVCTL;
    msg.i.combine_len = sizeof msg.i;
    msg.i.devctl_cmd = devctl_cmd;
    msg.i.nbytes = (int32_t) nbytes;
    msg.i.zero = 0;

    /* Setup data to the device. */
    SETIOV(iov + 0, &msg.i, sizeof msg.i);
    SETIOV(iov + 1, data_ptr,
           ((unsigned) devctl_cmd & _POSIX_DEVDIR_TO) ? nbytes : 0);

    /* Setup data from the device. */
    SETIOV(iov + 2, &msg.o, sizeof msg.o);
    SETIOV(iov + 3, data_ptr,
           ((unsigned) devctl_cmd & _POSIX_DEVDIR_FROM) ? nbytes : 0);

    if ((status = msg_sendv_r(fd, iov + 0, GETIOVLEN(&iov[1]) ? 2 : 1, iov + 2,
                              GETIOVLEN(&iov[3]) ? 2 : 1)) != EOK) {
        return status == -ENOSYS ? ENOTTY : -status;
    }

    if (info_ptr) {
        *info_ptr = msg.o.ret_val;
    }

    return EOK;
}
