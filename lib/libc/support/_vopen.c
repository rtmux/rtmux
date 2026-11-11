/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <fcntl.h>
#include <sys/iomsg.h>
#include <sys/stat.h>

__hidden int _vopen(const char *path, int oflag, int sflag, va_list ap)
{
    int mode = 0;

    if (oflag & O_CREAT)
        mode = va_arg(ap, int) & ~S_IFMT;

    return _connect(0, path, (mode_t) mode, (unsigned) oflag, (unsigned) sflag,
                    _IO_CONNECT_OPEN, 1, _IO_FLAG_RD | _IO_FLAG_WR, 0, 0, 0, 0,
                    0, 0, 0);
}
