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

void _exit(int status)
{
    struct _proc_exit msg;
    struct _proc_exit_reply reply;
    iov_t siov, riov;

    msg.type = _PROC_EXIT;
    msg.zero = 0;
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

void _Exit(int status)
{
    _exit(status);
}
