/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

int setpgid(pid_t pid, pid_t pgid)
{
    proc_setpgid_t msg;

    msg.i.type = _PROC_SETPGID;
    msg.i.pid = pid;
    msg.i.pgid = pgid;
    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0);
}
