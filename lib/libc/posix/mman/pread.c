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

ssize_t pread(int fd, void *buff, size_t nbytes, off_t offset)
{
    struct read_offset {
        struct _io_read read;
        struct _xtype_offset xoffset;
    } msg = {0}; /* don't leak stack padding to the server */

    if (offset < 0) {
        errno = EINVAL;
        return -1;
    }

    /* msg_send() returns the byte count as int; a larger transfer cannot be
     * represented on return.
     */
    if (nbytes > INT32_MAX) {
        errno = EINVAL;
        return -1;
    }

    msg.read.type = _IO_READ;
    msg.read.combine_len =
        (uint16_t) (offsetof(struct read_offset, xoffset) + sizeof msg.xoffset);
    msg.read.nbytes = nbytes;
    msg.read.xtype = _IO_XTYPE_OFFSET;
    msg.read.zero = 0;
    msg.xoffset.offset = offset;

    return msg_send(fd, &msg,
                    offsetof(struct read_offset, xoffset) + sizeof msg.xoffset,
                    buff, nbytes);
}
