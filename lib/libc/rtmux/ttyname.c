/*
 * ttyname / ttyname_r -- the pathname of a terminal.
 *
 * ttyname_r is reentrant (caller buffer, no static state); ttyname() is the
 * non-reentrant static-buffer wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

int ttyname_r(int fd, char *name, size_t namesize)
{
    struct stat tty;
    struct dirent *entry;
    char path[PATH_MAX] = "/dev/";
    int rc;

    if (fcntl(fd, F_GETFD) < 0)
        return errno;
    if (!isatty(fd))
        return ENOTTY;
    if (fstat(fd, &tty) < 0)
        return errno;
    DIR *dir = opendir("/dev");
    if (!dir)
        return errno;
    for (;;) {
        struct stat candidate;
        errno = 0;
        if (!(entry = readdir(dir))) {
            rc = errno ? errno : ENODEV;
            break;
        }
        size_t len = strlen(entry->d_name);
        if (len >= sizeof(path) - 5)
            continue;
        memcpy(path + 5, entry->d_name, len + 1);
        /* Return the device node itself even if /dev/console links to it. */
        if (lstat(path, &candidate) || !S_ISCHR(candidate.st_mode) ||
            candidate.st_rdev != tty.st_rdev)
            continue;
        rc = len + 6 > namesize ? ERANGE : 0;
        if (!rc)
            memcpy(name, path, len + 6);
        break;
    }
    closedir(dir);
    return rc;
}

char *ttyname(int fd)
{
    static char buf[PATH_MAX];
    int rc = ttyname_r(fd, buf, sizeof buf);

    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    return buf;
}
