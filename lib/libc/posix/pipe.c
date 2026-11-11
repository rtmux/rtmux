/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/iomsg.h>
#include <sys/ftype.h>

#include <posix.h>

int pipe(int fildes[2])
{
    if ((fildes[0] = _connect(0, "/dev/pipe", 0, O_RDONLY | O_NOCTTY, 0,
                              _IO_CONNECT_OPEN, 0, _IO_FLAG_RD | _IO_FLAG_WR,
                              _FTYPE_PIPE, 0, 0, 0, 0, 0, 0)) != -1) {
        if ((fildes[1] = __rtmux_openfd_typed(fildes[0], O_WRONLY, 0,
                                              _IO_OPENFD_PIPE)) != -1) {
            return 0;
        }
        close(fildes[0]);
    } else if (errno == ENOENT) {
        errno = ENOSYS;
    }
    return -1;
}
