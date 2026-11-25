/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system-channel.c — handle system channel registration.
 *
 * When a process starts, crt0 sends a _PROC_SYSCHAN message to roottask with
 * the chid of its system channel. Roottask looks up the channel and stores the
 * kernel pointer in prp->sys_chp. This is used by the kernel to deliver
 * notifications (process termination, shutdown, etc.) directly via
 * notification_deliver().
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "root-log.h"

/* Wrapper for __KSYS_PRIV_PROC_REGISTER_SYSCHAN. Lives in kext-wrappers.c once
 * we've grown enough of these; for now keep the call site local.
 */
struct kcall_args_proc_register_syschan {
    pid_t pid;
    int chid;
};

/* proc_syschan — handle a _PROC_SYSCHAN message.
 *
 * Registers the calling process's system channel with the kernel (via
 * __KSYS_PRIV_PROC_REGISTER_SYSCHAN) so the kernel can deliver termination and
 * shutdown notifications to it directly, delivers the signals that were sent to
 * the process before it had one (signal_flush_early_locked), then replies
 * success.
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = calling process)
 *   msg : the syschan request (msg->i.chid = system-channel chid)
 * Return value: IOSERV_NOREPLY on success, or an errno on failure.
 */
int proc_syschan(ioserv_context_t *ctp, proc_syschan_t *msg)
{
    struct kcall_args_proc_register_syschan args;
    process_t *prp;
    int r, killed = 0;

    args.pid = ctp->info.pid;
    args.chid = msg->i.chid;

    /* Register and deliver the signals sent before now under the process's
     * lock, the one signal_one posts under: a signal either finds the channel
     * or is kept and flushed here, never neither.
     */
    if (!(prp = root_proc_lock(ctp->info.pid)))
        return ESRCH;
    r = (int) syscall_priv(__KSYS_PRIV_PROC_REGISTER_SYSCHAN, &args);
    if (r == EOK)
        killed = signal_flush_early_locked(ctp->info.pid);
    root_proc_unlock(ctp->info.pid);
    if (killed)
        proc_exit_finish(ctp->info.pid);
    if (r != EOK) {
        root_log("proc_syschan: pid %d chid %d not found (r=%d)\n",
                 ctp->info.pid, msg->i.chid, r);
        return r;
    }

    root_trace("proc_syschan: pid %d registered system channel (chid=%d)\n",
               ctp->info.pid, msg->i.chid);

    /* Reply with success */
    msg->o.zero = EOK;
    msg_reply(ctp->rcvid, EOK, &msg->o, sizeof(msg->o));
    return IOSERV_NOREPLY;
}
