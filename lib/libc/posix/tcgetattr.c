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

int tcgetattr(int fd, struct termios *termios_p)
{
    return _devctl(fd, DEVCTL_TTY_GET_ATTR, termios_p, sizeof *termios_p,
                   _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY);
}
