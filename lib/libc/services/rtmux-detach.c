/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* rtmux_detach() — detach from parent process (stay resident).
 *
 * Sends _PROC_DETACH to roottask. On success, the parent's waitpid() is
 * unblocked with the given status and this process is reparented to pid 1. The
 * process continues running.
 *
 * License: Apache 2.0
 */

#include <errno.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

int rtmux_detach(int status)
{
    proc_detach_t msg;

    msg.i.type = _PROC_DETACH;
    msg.i.zero = 0;
    msg.i.status = status;
    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0);
}
