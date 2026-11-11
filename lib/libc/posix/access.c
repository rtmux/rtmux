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
#include <sys/types.h>
#include <sys/mount.h>
#include <sys/iomsg.h>
#include <devctl/common.h>
#include <sys/rtmux-core.h>

#include <posix.h>

#if (R_OK != S_IROTH || W_OK != S_IWOTH || X_OK != S_IXOTH)
#error access() implementation requires *_OK and S_I*OTH equivalence
#endif

static int __access(const char *path, int amode, int oflag, mode_t symmode)
{
    int fd;
    unsigned mode;
    int n;
    struct _io_stat s;
    struct stat r;
    struct _client_info info;

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    /* Only 1003.1a valid modes allowed */
    if ((amode & (F_OK | R_OK | W_OK | X_OK)) != amode) {
        errno = EINVAL;
        return -1;
    }

    /* get the stat info. symmode is S_IFLNK to stat the symlink itself (no
     * follow, for faccessat's AT_SYMLINK_NOFOLLOW) or 0 to follow it.
     */
    s.type = _IO_STAT;
    s.combine_len = sizeof s;
    s.zero = 0;
    if ((fd = _connect(RTMUX_SIDE_CHANNEL, path, symmode,
                       (unsigned) oflag | O_NONBLOCK | O_NOCTTY, 0,
                       _IO_CONNECT_COMBINE, 0, 0, 0, 0, sizeof s, &s, sizeof r,
                       &r, 0)) == -1) {
        return -1;
    }

    /* Use realids instead of effective */
    connect_client_info(-1, &info, NGROUPS_MAX);
    if (oflag & O_REALIDS) {
        info.cred.euid = info.cred.ruid;
        info.cred.egid = info.cred.rgid;
    }

    if (info.cred.euid == 0) {
        /* super-user always gets read-write access and execute access if any x
         * bit is set and directory search access
         */
        mode =
            (S_ISDIR(r.st_mode) || (r.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)))
                ? S_IROTH | S_IWOTH | S_IXOTH
                : S_IROTH | S_IWOTH;
    } else if (info.cred.euid == r.st_uid) {
        mode = (r.st_mode >> 6) & 07;
    } else if (info.cred.egid == r.st_gid) {
        mode = (r.st_mode >> 3) & 07;
    } else {
        for (n = 0; (unsigned) n < info.cred.ngroups &&
                    info.cred.grouplist[n] != r.st_gid;
             ++n)
            ;
        mode = ((unsigned) n >= info.cred.ngroups) ? r.st_mode & 07
                                                   : (r.st_mode >> 3) & 07;
    }

    mode &= R_OK | W_OK | X_OK;
    if ((mode | (unsigned) amode) != mode) {
        close(fd);
        errno = EACCES;
        return -1;
    }

    /* If not a directory, check mount flags */
    if (amode & (W_OK | X_OK)) {
        unsigned flags;

        if (_devctl(fd, DEVCTL_GET_MOUNT_FLAGS, &flags, sizeof flags, 0) !=
            EOK) {
            /* Doesn't support mount flags, so default to NOEXEC */
            flags = _MOUNT_NOEXEC;
        }

        /* Check read-only file system */
        if ((amode & W_OK) && (flags & _MOUNT_READONLY)) {
            close(fd);
            errno = EROFS;
            return -1;
        }

        /* Check mounted noexec */
        if (!S_ISDIR(r.st_mode) && (amode & X_OK) && (flags & _MOUNT_NOEXEC)) {
            close(fd);
            errno = EACCES;
            return -1;
        }
    }

    close(fd);
    return 0;
}

int access(const char *path, int amode)
{
    return __access(path, amode, O_REALIDS, 0);
}

int faccessat(int dirfd, const char *path, int amode, int flag)
{
    char abuf[PATH_MAX + 1];
    const char *rpath = path;

    if (!path) {
        errno = EFAULT;
        return -1;
    }

    /* AT_EACCESS checks with the effective ids instead of the real ids access()
     * uses; AT_SYMLINK_NOFOLLOW checks the symlink itself rather than its
     * target. No other flag is defined. __access() validates amode.
     */
    if (flag & ~(AT_EACCESS | AT_SYMLINK_NOFOLLOW)) {
        errno = EINVAL;
        return -1;
    }
    if (dirfd != AT_FDCWD && path[0] != '/') {
        if (__atfd_abspath(dirfd, path, abuf, sizeof abuf) != 0)
            return -1;
        rpath = abuf;
    }

    return __access(rpath, amode, (flag & AT_EACCESS) ? 0 : O_REALIDS,
                    (flag & AT_SYMLINK_NOFOLLOW) ? S_IFLNK : 0);
}
