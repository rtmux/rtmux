/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/wait-ops.h>

#define WAIT_CONTINUED_STATUS 0xffff
#define WAIT_CORE_FLAG 0x0080
#define WAIT_STOP_FLAG 0x007f
#define WAIT_SIGNAL_MASK 0x007f

static pid_t wait_child(pid_t pid, int *stat_loc, int options)
{
    idtype_t idtype;

    if (pid == 0) {
        idtype = P_ALL;
    } else if (pid <= 0) {
        pid = -pid;
        idtype = P_PGID;
    } else {
        idtype = P_PID;
    }

    siginfo_t info;
    if (waitid(idtype, pid, &info, options | WEXITED | RTMUX_WTRAPPED) == -1)
        return -1;
    if (info.si_signo != SIGCHLD)
        return 0;
    if (!stat_loc)
        return info.si_pid;

    switch (info.si_code) {
    case CLD_EXITED:
        *stat_loc = (info.si_status & 0xff) << 8;
        break;
    case CLD_KILLED:
        *stat_loc = info.si_status & WAIT_SIGNAL_MASK;
        break;
    case CLD_DUMPED:
        *stat_loc = (info.si_status & WAIT_SIGNAL_MASK) | WAIT_CORE_FLAG;
        break;
    case CLD_TRAPPED:
    case CLD_STOPPED:
        *stat_loc = ((info.si_status & WAIT_SIGNAL_MASK) << 8) | WAIT_STOP_FLAG;
        break;
    case CLD_CONTINUED:
        *stat_loc = WAIT_CONTINUED_STATUS;
        break;
    default:
        errno = EINVAL;
        return -1;
    }

    return info.si_pid;
}

pid_t waitpid(pid_t pid, int *stat_loc, int options)
{
    if (pid == -1) {
        pid = 0;
    } else if (pid == 0) {
        pid = -getpgrp();
    }
    return wait_child(pid, stat_loc, options);
}

pid_t wait(int *stat_loc)
{
    return waitpid(-1, stat_loc, 0);
}
