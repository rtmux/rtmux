/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>

#include <posix.h>
#include <sys/loader-fd.h>

static int open_path(const char *path, int oflag, va_list ap, int track)
{
    int matched = 0;
    int fd = __rtmux_procfd_open(path, oflag, 0, &matched);
    if (!matched)
        fd = _vopen(path, oflag, 0, ap);
    if (track && fd != -1 && !matched)
        __atfd_track_open(fd, path);
    return fd;
}

int open(const char *path, int oflag, ...)
{
    va_list ap;
    va_start(ap, oflag);
    int fd = open_path(path, oflag, ap, 1);
    va_end(ap);
    return fd;
}

int __loader_open(const char *path, int flags, ...)
{
    va_list ap;
    va_start(ap, flags);
    int fd = open_path(path, flags, ap, 0);
    va_end(ap);
    return fd;
}

int openat(int dirfd, const char *path, int oflag, ...)
{
    char abuf[PATH_MAX + 1];
    const char *rpath;
    va_list ap;
    int fd;
    int matched = 0;

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    /* A relative path against a real dirfd resolves against the directory that
     * descriptor was opened as; an absolute path or AT_FDCWD opens normally. An
     * untracked dirfd fails with EBADF/ENOTDIR/ENOSYS precedence.
     */
    if (dirfd != AT_FDCWD && path[0] != '/') {
        if (__atfd_abspath(dirfd, path, abuf, sizeof abuf) != 0)
            return -1;
        rpath = abuf;
    } else {
        rpath = path;
    }

    va_start(ap, oflag);
    fd = __rtmux_procfd_open(rpath, oflag, 0, &matched);
    if (!matched)
        fd = _vopen(rpath, oflag, 0, ap);
    va_end(ap);
    if (fd != -1 && !matched)
        __atfd_track_open(fd, rpath);
    return fd;
}
