/*
 * lseek() — repositions file offset via msg_send _IO_LSEEK.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/iomsg.h>

off_t lseek(int fd, off_t offset, int whence)
{
    io_lseek_t msg = {
        .i =
            {
                .type = _IO_LSEEK,
                .combine_len = sizeof msg.i,
                .offset = (uint64_t) offset,
                .whence = (short) whence,
                .zero = 0,
            },
    };
    off_t result;

    if (msg_send(fd, &msg.i, sizeof msg.i, &result, sizeof result) == -1)
        return (off_t) -1;

    return result;
}
