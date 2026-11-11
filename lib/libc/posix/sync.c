/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/sysmsg.h>

/* Ask roottask to have every mounted server flush to stable storage. POSIX
 * gives sync() no failure indication, so a failed or refused sweep is silent.
 */
void sync(void)
{
    sys_cmd_t msg;

    memset(&msg, 0, sizeof msg);
    msg.i.type = _SYS_CMD;
    msg.i.cmd = _SYS_CMD_SYNC;
    (void) msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, NULL, 0);
}
