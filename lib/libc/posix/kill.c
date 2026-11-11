/*
 * kill() — send signal to process via notification.
 *
 * In RTMux, signals are delivered as notifications through the process's system
 * channel. kill() sends a _PROC_KILL message to roottask, which delivers a
 * _NOTIFICATION_CODE_SIGNAL notification to the target.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <errno.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

#include "signal-state.h"

int kill(pid_t pid, int sig)
{
    proc_kill_t msg;

    if (sig < 0 || sig >= NSIG) {
        errno = EINVAL;
        return -1;
    }
    if (pid == getpid()) {
        if (sig == 0)
            return 0;
        return __libc_signal_raise(sig);
    }

    msg.i.type = _PROC_KILL;
    msg.i.zero = 0;
    msg.i.pid = (int32_t) pid;
    msg.i.signo = (int32_t) sig;
    return msg_sendnc(ROOTTASK_COID, &msg.i, sizeof msg.i, 0, 0);
}

int killpg(pid_t pgrp, int sig)
{
    /* kill(-1) is the broadcast form, so group 1 cannot be named through it. */
    if (pgrp < 0 || pgrp == 1) {
        errno = EINVAL;
        return -1;
    }
    return pgrp == 0 ? kill(0, sig) : kill(-pgrp, sig);
}
