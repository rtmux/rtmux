/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <fcntl.h>
#include <sys/iomsg.h>

int fsync(int fd)
{
    io_sync_t msg;

    msg.i.type = _IO_SYNC;
    msg.i.combine_len = sizeof msg.i;
    msg.i.flag = O_SYNC;
    return msg_sendnc(fd, &msg.i, sizeof msg.i, 0, 0);
}
