/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/iomsg.h>

#include <posix.h>

int unlink(const char *path)
{
    int fd;
    int status;

    fd = _connect(RTMUX_SIDE_CHANNEL, path, S_IFLNK, O_NOCTTY, 0,
                  _IO_CONNECT_UNLINK, 0, 0, 0, 0, 0, 0, 0, 0, &status);
    if (fd == -1)
        return -1;
    __connect_detach((uint64_t) fd);
    return status;
}

int unlinkat(int dirfd, const char *path, int flag)
{
    char abuf[PATH_MAX + 1];
    const char *rpath = path;

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    /* AT_REMOVEDIR is a valid unlinkat flag, but RTMux exposes no native
     * directory removal. Classify the target first, then fail closed only for
     * actual directories.
     */
    if (flag & ~AT_REMOVEDIR) {
        errno = EINVAL;
        return -1;
    }
    if (dirfd != AT_FDCWD && path[0] != '/') {
        if (__atfd_abspath(dirfd, path, abuf, sizeof abuf) != 0)
            return -1;
        rpath = abuf;
    }
    if (flag & AT_REMOVEDIR) {
        struct stat st;

        if (stat(rpath, &st) == -1)
            return -1;
        if (!S_ISDIR(st.st_mode)) {
            errno = ENOTDIR;
            return -1;
        }
        errno = ENOSYS;
        return -1;
    }
    return unlink(rpath);
}
