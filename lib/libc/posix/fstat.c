/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/iomsg.h>

#include <posix.h>

/* __stat_check: RTMux is always LP64 / _FILE_OFFSET_BITS==64, so only a basic
 * sanity check is needed.
 */
int __stat_check(const struct stat *st)
{
    if (st->st_size < 0) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

int fstat(int fd, struct stat *statl)
{
    io_stat_t msg;

    msg.i.type = _IO_STAT;
    msg.i.combine_len = sizeof msg.i;
    msg.i.zero = 0;
    if (msg_sendnc(fd, &msg.i, sizeof msg.i, statl, sizeof *statl) == -1)
        return -1;
    return __stat_check(statl);
}
