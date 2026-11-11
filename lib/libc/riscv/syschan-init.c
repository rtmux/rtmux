/*
 * RTMux — system channel initialization and system thread
 *
 * Called from crt0 before main(). Creates the per-process system channel,
 * spawns the system thread, and registers the channel with roottask so that
 * notifications can be delivered to this process.
 *
 * The system thread receives notifications via msg_receive_notification
 * (notification-only, does NOT participate in thread pool management). Signal
 * notifications are dispatched through libc signal state.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdalign.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/syspage.h>
#include <sys/procmsg.h>
#include <sys/_pthreadtypes.h>
#include <pthread.h>
#include <errno.h>

#include "signal-state.h"

static int __rtmux_syschan_chid = -1;

/* Start of the auxiliary vector, for issetugid(). Any start file that sets
 * environ to the initial envp before calling __rtmux_syschan_init() gets it
 * recorded here: the vector follows envp's NULL terminator.
 */
const uint64_t *__libc_auxv __hidden;
extern char **environ __attribute__((weak));

#define SYSTHRD_STACK_SIZE 4096

/* Static stack for the system thread — avoids needing malloc before main() */
static alignas(16) char __systhrd_stack[SYSTHRD_STACK_SIZE];

/* System thread entry point. Receives notifications only
 * (msg_receive_notification). Does NOT use msg_receive — that would trigger
 * pool thread growth.
 */
static void __systhrd_entry(void)
{
    struct _notification notification;

    if (__rtmux_syschan_chid < 0)
        return;

    for (;;) {
        int rcvid = msg_receive_notification(
            __rtmux_syschan_chid, &notification, sizeof(notification), NULL);
        if (rcvid == -1)
            return;

        if (notification.code == _NOTIFICATION_CODE_SIGNAL) {
            /* Low 32 bits: signo; high 32 bits: sender pid (see
             * kext_signal_deliver).
             */
            uint64_t val = (uint64_t) (uintptr_t) notification.value.sival_ptr;
            int signo = (int) (uint32_t) val;
            pid_t sender = (pid_t) (uint32_t) (val >> 32);

            /* Hand off to the libc signal table. The dispatcher runs the
             * registered handler from this (system) thread if the signal is
             * unblocked, or marks it pending otherwise. See
             * lib/libc/rtmux/signal-state.c.
             */
            __libc_signal_notification_recv(signo, sender);
        }
    }
}

extern long SysCpupageGet_r(int index) __hidden;
extern struct syspage_entry *_syspage_ptr;

void __rtmux_syschan_init(void)
{
    int chid, coid;
    struct _proc_syschan msg;
    struct _proc_syschan_reply reply;
    iov_t siov, riov;
    static struct _thread_attr attr;

    /* Reset the signal table to defaults. Safe to call before the system
     * channel exists — the table is process-wide and lives independently of the
     * channel.
     */
    __libc_signal_init();

    if (&environ && environ) {
        char **ep = environ;

        while (*ep)
            ep++;
        __libc_auxv = (const uint64_t *) (ep + 1);
    }

    /* Initialize the syspage pointer for this process. */
    if (!_syspage_ptr)
        _syspage_ptr =
            (struct syspage_entry *) SysCpupageGet_r(CPUPAGE_SYSPAGE);

    /* Roottask (pid 1) does not need a system channel — it IS the system */
    if (getpid() == ROOTTASK_PID)
        return;

    /* Create the system channel Fixed priority: a posted signal must not
     * rewrite the system thread's priority, or every handler would run at the
     * kernel's notification priority, behind every realtime thread.
     */
    chid = channel_create(RTMUX_FLG_CHF_FIXED_PRIORITY);
    if (chid == -1)
        return;

    __rtmux_syschan_chid = chid;

    /* Connect to roottask to register the system channel */
    coid = connect_attach(0, ROOTTASK_PID, ROOTTASK_CHID,
                          RTMUX_SIDE_CHANNEL | RTMUX_FLG_COF_CLOEXEC, 0);
    if (coid == -1)
        return;

    /* Tell roottask about our system channel */
    msg.type = _PROC_SYSCHAN;
    msg.subtype = 0;
    msg.chid = chid;

    SETIOV(&siov, &msg, sizeof(msg));
    SETIOV(&riov, &reply, sizeof(reply));

    msg_sendv(coid, &siov, 1, &riov, 1);

    /* Detach the temporary connection — roottask has stored the kernel channel
     * pointer in prp->sys_chp
     */
    __connect_detach((uint64_t) coid);

    /* Spawn the system thread */
    memset(&attr, 0, sizeof(attr));
    attr.stackaddr = __systhrd_stack;
    attr.stacksize = SYSTHRD_STACK_SIZE;
    attr.flags = PTHREAD_CREATE_DETACHED;

    /* Inherit the startup thread's scheduling policy and priority, and never
     * change it (the channel is FIXED_PRIORITY): handlers run at the priority
     * the process was started with, not at the kernel's notification priority.
     */
    __thread_create(0, (void *) __systhrd_entry, NULL, &attr);
}
