/*
 * ttyname / ttyname_r -- the pathname of a terminal.
 *
 * RTMux runs a single console terminal, /dev/console (created by the logger;
 * getty and the login shell inherit it as their standard streams). The tty
 * devctl surface has no name query and RTMux's service-backed devices carry no
 * traditional device numbers to scan /dev against, so any terminal descriptor
 * is reported as /dev/console. A multi-terminal ttyname() would need a
 * name-query devctl RTMux does not provide.
 *
 * ttyname_r is reentrant (caller buffer, no static state); ttyname() is the
 * non-reentrant static-buffer wrapper.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

int ttyname_r(int fd, char *name, size_t namesize)
{
    static const char dev[] = "/dev/console";

    if (fcntl(fd, F_GETFD) == -1)
        return errno; /* EBADF: not an open descriptor */
    if (!isatty(fd))
        return ENOTTY;
    if (sizeof dev > namesize) /* sizeof counts the NUL */
        return ERANGE;
    memcpy(name, dev, sizeof dev);
    return 0;
}

char *ttyname(int fd)
{
    static char buf[sizeof "/dev/console"];
    int rc = ttyname_r(fd, buf, sizeof buf);

    if (rc != 0) {
        errno = rc;
        return NULL;
    }
    return buf;
}
