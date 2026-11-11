/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/procmsg.h>
#include <sys/rtmux-core.h>

int tcsetsid(int fd, pid_t pid)
{
    proc_session_t msg;

    msg.i.type = _PROC_SESSION;
    msg.i.subtype = 0;
    msg.i.sid = pid;
    msg.i.id = fd;
    msg.i.event = _PROC_SESSION_TCSETSID;
    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0);
}
