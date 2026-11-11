/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <unistd.h>
#include <devctl.h>
#include <devctl/common.h>
#include <sys/statvfs.h>

int fstatvfs(int fd, struct statvfs *buf)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }

    int rc = devctl(fd, DEVCTL_STATVFS, buf, sizeof(*buf), NULL);
    if (rc != EOK) {
        errno = rc;
        return -1;
    }
    return 0;
}

int statvfs(const char *path, struct statvfs *buf)
{
    if (!path || !buf) {
        errno = EFAULT;
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return -1;
    if (fstatvfs(fd, buf) == 0) {
        (void) close(fd);
        return 0;
    }
    int saved_errno = errno;
    (void) close(fd);
    errno = saved_errno;
    return -1;
}
