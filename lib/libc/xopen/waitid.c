/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* waitid — wait for a child process to change state.
 *
 * Uses the proc service _PROC_WAIT path for ECHILD, child enumeration, and
 * deferred replies for children that have not exited yet.
 */

#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>
#include "signal-state.h"

int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options)
{
    proc_wait_t msg;
    iov_t iov[2];
    libc_waiter_t w;
    int r;

    msg.i.type = _PROC_WAIT;
    msg.i.idtype = idtype;
    msg.i.id = id;
    msg.i.options = options;
    SETIOV(iov + 0, &msg.i, sizeof msg.i);
    SETIOV(iov + 1, infop, sizeof *infop);
    __libc_wait_enter(&w);
    r = msg_sendv(ROOTTASK_COID, iov + 0, 1, iov + 1, infop ? 1 : 0);
    __libc_wait_exit(&w);
    return r;
}
