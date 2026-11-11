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

int tcsetattr(int fd, int optact, const struct termios *termios_p)
{
    unsigned devctl_cmd;

    switch (optact) {
    case TCSANOW:
        devctl_cmd = DEVCTL_TTY_SET_ATTR;
        break;
    case TCSADRAIN:
        devctl_cmd = DEVCTL_TTY_SET_ATTR_DRAIN;
        break;
    case TCSAFLUSH:
        devctl_cmd = DEVCTL_TTY_SET_ATTR_FLUSH;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    return _devctl(fd, devctl_cmd, (void *) termios_p, sizeof *termios_p,
                   _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY);
}
