/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/iomsg.h>
#include <unistd.h>
#include "externs.h"

/* Read bytes from a file descriptor at an absolute offset.
 *
 * Issues _IO_READ messages with _IO_XTYPE_OFFSET so each chunk is independent
 * of the connection's mutable seek position. Parameters:
 *   fd     : the file descriptor (connection) to read from
 *   buff   : destination buffer
 *   nbytes : number of bytes requested
 *   off    : absolute file offset to read from
 * Return value: the number of bytes read, or -1 with errno set on
 *               failure.
 */
ssize_t proc_read(int fd, void *buff, ssize_t nbytes, off64_t off)
{
    struct offset_read {
        struct _io_read read;
        struct _xtype_offset offset;
    } msg;
    ssize_t received;
    ssize_t got;

    msg.read.type = _IO_READ;
    msg.read.combine_len = sizeof msg;
    msg.read.xtype = _IO_XTYPE_OFFSET;
    msg.read.zero = 0;
    received = 0;

    for (;;) {
        if (received >= nbytes)
            break;
        msg.read.nbytes = (size_t) (nbytes - received);
        msg.offset.offset = off + received;
        /* ipc-send: roottask-fsio -> romfs,lkl-fsd,dev-nvme,pipe */
        got = msg_send(fd, &msg, sizeof msg, (uint8_t *) buff + received,
                       msg.read.nbytes);
        if (got == -1) {
            root_dbg(
                "proc_read: msg_send(fd=%d, buff=%p, nbytes=%zu, off=%lld"
                ") FAILED errno=%d\n",
                fd, (uint8_t *) buff + received, msg.read.nbytes,
                (long long) msg.offset.offset, errno);
            return -1;
        }
        if (got == 0)
            break;
        if (got > (ssize_t) msg.read.nbytes)
            got = (ssize_t) msg.read.nbytes;
        received += got;
    }

    return received;
}
