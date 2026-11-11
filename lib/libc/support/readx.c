/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <unistd.h>
#include <sys/iomsg.h>
#include <sys/uio.h>

ssize_t _readx(int fd,
               void *buff,
               size_t nbytes,
               unsigned xtype,
               void *xdata,
               size_t xdatalen)
{
    iov_t iov[2];
    struct _io_read msg;

    if ((int) xdatalen < 0) {
        errno = EINVAL;
        return -1;
    }

    msg.type = _IO_READ;
    msg.combine_len = sizeof msg;
    msg.nbytes = nbytes;
    msg.xtype = (uint16_t) xtype;
    msg.zero = 0;

    SETIOV(iov + 1, buff, nbytes);
    SETIOV(iov + 0, xdata, xdatalen);

    return msg_sendv(fd, (void *) &msg, -(int) sizeof msg, iov, 2);
}
