/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <unistd.h>
#include <inttypes.h>
#include <errno.h>
#include <sys/iomsg.h>

ssize_t pwrite(int fd, const void *buff, size_t nbytes, off_t offset)
{
    struct write_offset {
        struct _io_write write;
        struct _xtype_offset xoffset;
    } msg = {0}; /* don't leak stack padding to the server */
    iov_t iov[2];

    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }

    /* nbytes lands in a signed 32-bit field; a larger count would advertise a
     * negative length while the iov still carries the full buffer.
     */
    if (nbytes > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    msg.write.type = _IO_WRITE;
    msg.write.combine_len = (uint16_t) (offsetof(struct write_offset, xoffset) +
                                        sizeof msg.xoffset);
    msg.write.nbytes = (int32_t) nbytes;
    msg.write.xtype = _IO_XTYPE_OFFSET;
    msg.write.zero = 0;
    msg.xoffset.offset = offset;

    SETIOV(iov + 0, &msg,
           offsetof(struct write_offset, xoffset) + sizeof msg.xoffset);
    SETIOV(iov + 1, buff, nbytes);
    return msg_sendv(fd, iov, 2, 0, 0);
}
