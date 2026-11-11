/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"

/* proc_umask — handle a _PROC_UMASK message (umask() get/set).
 *
 * Returns the target process's current umask and, on a SET request, installs
 * the new value (only the caller, or root for another process, may set it).
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = calling process)
 *   msg : the umask request (subtype, target pid, new umask)
 * Return value: a IOSERV_* status code, or an errno on failure.
 */
int proc_umask(ioserv_context_t *ctp, proc_umask_t *msg)
{
    process_t *prp;
    unsigned old_umask;

    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
        ENDIAN_SWAP16(&msg->i.subtype);
        ENDIAN_SWAP32(&msg->i.umask);
        ENDIAN_SWAP32(&msg->i.pid);
    }
    switch (msg->i.subtype) {
    case _PROC_UMASK_GET:
    case _PROC_UMASK_SET: {
        pid_t target_pid = msg->i.pid ? (pid_t) msg->i.pid : ctp->info.pid;
        root_process_t *tmp = root_proc(target_pid);
        if (!(prp = root_proc_lock(target_pid))) {
            return ESRCH;
        }
        old_umask = tmp->umask;
        if (msg->i.subtype == _PROC_UMASK_SET) {
            if (msg->i.pid != 0 && !proc_isaccess(prp, &ctp->info)) {
                return proc_error(EPERM, target_pid);
            }
            tmp->umask = msg->i.umask;
        }
        root_proc_unlock(target_pid);
        break;
    }
    default:
        return ENOSYS;
    }
    msg->o.zero1 = 0;
    msg->o.umask = old_umask;
    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
        ENDIAN_SWAP32(&msg->o.umask);
    }
    SETIOV(ctp->iov + 0, &msg->o, sizeof msg->o);
    return IOSERV_NPARTS(1);
}
