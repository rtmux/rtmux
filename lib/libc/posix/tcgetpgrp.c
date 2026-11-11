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
#include <sys/types.h>

int tcgetpgrp(int fd)
{
    pid_t pgrp;

    if (_devctl(fd, DEVCTL_TTY_GET_PGRP, &pgrp, sizeof pgrp,
                _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY) == -1)
        return -1;
    return pgrp;
}
