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
#include <sys/procmsg.h>
#include <sys/rtmux-core.h>
#include <unistd.h>

int tcsetpgrp(int fd, pid_t pgrp)
{
    proc_session_t msg;

    if (pgrp <= 0) {
        errno = EINVAL;
        return -1;
    }

    if (tcgetpgrp(fd) == -1)
        return -1;

    msg.i.type = _PROC_SESSION;
    msg.i.subtype = 0;
    msg.i.sid = 0;
    msg.i.id = pgrp;
    msg.i.event = _PROC_SESSION_VALIDATE_PGRP;
    if (msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0) == -1)
        return -1;

    return _devctl(fd, DEVCTL_TTY_SET_PGRP, &pgrp, sizeof pgrp,
                   _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY);
}
