/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/rtmux-core.h>
#include <sys/storage.h>
#include <sys/procmsg.h>
#include <sys/stat.h>

static mode_t umask_pid(pid_t pid, mode_t cmask)
{
    proc_umask_t msg = {
        .i.type = _PROC_UMASK,
        .i.subtype = _PROC_UMASK_SET,
        .i.umask = (uint32_t) cmask & 0777U,
        .i.pid = pid,
    };

    if (msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, &msg.o, sizeof msg.o) ==
        -1) {
        return (mode_t) -1;
    }
    return (mode_t) msg.o.umask;
}

mode_t umask(mode_t cmask)
{
    return umask_pid(0, cmask);
}
