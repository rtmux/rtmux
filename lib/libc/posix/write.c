/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Adapted for RTMux. */

#include <unistd.h>
#include <errno.h>
#include <inttypes.h>
#include <sys/iomsg.h>
#include "signal-state.h"

ssize_t write(int fd, const void *buff, size_t nbytes)
{
    io_write_t msg;
    iov_t iov[2];
    libc_waiter_t w;
    ssize_t r;

    if (nbytes > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    msg.i.type = _IO_WRITE;
    msg.i.combine_len = sizeof msg.i;
    msg.i.xtype = _IO_XTYPE_NONE;
    msg.i.nbytes = (int32_t) nbytes;
    msg.i.zero = 0;
    SETIOV(iov + 0, &msg.i, sizeof msg.i);
    SETIOV(iov + 1, buff, nbytes);
    __libc_wait_enter(&w);
    r = msg_sendv(fd, iov, 2, 0, 0);
    __libc_wait_exit(&w);
    return r;
}
