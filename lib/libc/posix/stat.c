/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/iomsg.h>

#include "connect.h"
#include <posix.h>

static int _stat(const char *path, struct stat *statl, int mode)
{
    struct _io_stat s;

    s.type = _IO_STAT;
    s.combine_len = sizeof s;
    s.zero = 0;

    if (_connect_combine(path, (mode_t) mode, O_NONBLOCK | O_NOCTTY, 0, 0, 0,
                         sizeof s, &s, sizeof *statl, statl) == -1)
        return -1;
    return __stat_check(statl);
}

int lstat(const char *path, struct stat *statl)
{
    return _stat(path, statl, S_IFLNK);
}

int stat(const char *path, struct stat *statl)
{
    return _stat(path, statl, 0);
}

int fstatat(int dirfd, const char *path, struct stat *statl, int flag)
{
    char abuf[PATH_MAX + 1];
    const char *rpath = path;
    int mode;

    if (!path) {
        errno = EFAULT;
        return -1;
    }
    if (flag & ~AT_SYMLINK_NOFOLLOW) {
        errno = EINVAL;
        return -1;
    }
    if (dirfd != AT_FDCWD && path[0] != '/') {
        if (__atfd_abspath(dirfd, path, abuf, sizeof abuf) != 0)
            return -1;
        rpath = abuf;
    }

    mode = (flag & AT_SYMLINK_NOFOLLOW) ? S_IFLNK : 0;
    return _stat(rpath, statl, mode);
}
