/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <devctl.h>
#include <devctl/tty.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/rtmux-core.h>

int isatty(int fd)
{
    /* A negative or side-channel handle is not a POSIX file descriptor; reject
     * it EBADF before any device query, matching the dup family (a bare F_GETFD
     * would accept a side channel that POSIX fd paths must not).
     */
    if (fd < 0 || ((unsigned) fd & RTMUX_SIDE_CHANNEL)) {
        errno = EBADF;
        return 0;
    }
    if (_devctl(fd, DEVCTL_TTY_ISATTY, 0, 0, 0) == -1) {
        /* Not a terminal. POSIX.1 requires errno set on the 0 return: EBADF for
         * an invalid descriptor, ENOTTY for a valid non-terminal one. _devctl's
         * own errno is device-defined, so distinguish with a cheap F_GETFD
         * validity probe.
         */
        errno = (fcntl(fd, F_GETFD) == -1) ? EBADF : ENOTTY;
        return 0;
    }
    return 1;
}
