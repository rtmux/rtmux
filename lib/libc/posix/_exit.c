/*
 * _exit() — terminate the calling process.
 *
 * Sends a _PROC_EXIT message to the proc service, which handles all cleanup
 * (channels, FDs, timers, memory, threads).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/rtmux-core.h>
#include <sys/procmsg.h>

#include "signal-state.h"

static _Noreturn void proc_exit_message(int status, uint16_t by_signal)
{
    struct _proc_exit msg;
    struct _proc_exit_reply reply;
    iov_t siov, riov;

    msg.type = _PROC_EXIT;
    msg.by_signal = by_signal;
    msg.status = status;

    SETIOV(&siov, &msg, sizeof(msg));
    SETIOV(&riov, &reply, sizeof(reply));

    /* Send to ROOTTASK_COID (connection 0 to proc service, always exists). This
     * blocks until proc service handles it — the process is dead after this
     * point.
     */
    msg_sendvnc(ROOTTASK_COID, &siov, 1, &riov, 1);

    /* Should not reach here — proc service destroys us */
    for (;;)
        ;
}

_Noreturn void __libc_signal_exit(int signo)
{
    proc_exit_message(signo, 1);
}

void _exit(int status)
{
    proc_exit_message(status, 0);
}

void _Exit(int status)
{
    _exit(status);
}
