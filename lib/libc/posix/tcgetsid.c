/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <termios.h>
#include <devctl.h>
#include <devctl/tty.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>

/* The session leader of the session the terminal fd is the controlling terminal
 * of. A descriptor that is not a terminal fails ENOTTY, a bad one EBADF, and so
 * does a terminal that is not the caller's controlling terminal: the terminal
 * server says which session it is bound to, and that must be the caller's.
 */
pid_t tcgetsid(int fd)
{
    pid_t sid;

    if (_devctl(fd, DEVCTL_TTY_GET_SID, &sid, sizeof sid,
                _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY) == -1)
        return -1;
    if (sid <= 0 || sid != getsid(0)) {
        errno = ENOTTY;
        return -1;
    }
    return sid;
}
