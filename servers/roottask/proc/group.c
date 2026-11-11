/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"

/* proc_setpgid — handle a _PROC_SETPGID message (setpgid()).
 *
 * Moves the target process into the requested process group, enforcing the
 * POSIX rules (target must be the caller or a non-exec'd child in the same
 * session, and the destination group must be in that session). Locks are taken
 * child-then-parent to preserve the system-wide lock order and avoid deadlock.
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = calling process)
 *   msg : the setpgid request (i.pid target, i.pgid new group)
 * Return value: EOK / an errno, or a IOSERV_* status code.
 */
int proc_setpgid(ioserv_context_t *ctp, proc_setpgid_t *msg)
{
    process_t *prp1, *prp2, *prp3;
    pid_t pid1, pid2, pid3;
    int ret;

    /* If pgid is less than zero return EINVAL */
    if (msg->i.pgid < 0) {
        return EINVAL;
    }

    /* Note that we have to be careful in here to maintain the lock order used
     * elsewhere in proc - child then parent else we can deadlock. See PR 23567.
     */
    ret = EOK;
    prp3 = NULL;
    prp1 = NULL;
    pid1 = pid2 = pid3 = 0;
    if (msg->i.pid == 0 || msg->i.pid == ctp->info.pid) {
        pid1 = ctp->info.pid;
        if (!(prp1 = root_proc_lock(pid1))) {
            return EL2HLT;
        }
        prp2 = prp1;
        pid2 = pid1;
    } else {
        root_proc_info_t qinfo2;

        pid2 = (pid_t) msg->i.pid;
        /* Lock prp2 first. */
        if (!(prp2 = root_proc_lock(pid2))) {
            ret = ESRCH;
            goto out_unlock;
        }

        /* prp2 fields (parent / flags) live in the kernel-half process_t
         * (gated); read via the process_query_info snapshot.
         */
        if (process_query_info(pid2, &qinfo2) != EOK ||
            qinfo2.parent_pid == 0) {
            ret = ESRCH;
            goto out_unlock;
        }

        /* If pid is not a child of the calling process return ESRCH */
        if (qinfo2.parent_pid != ctp->info.pid) {
            ret = ESRCH;
            goto out_unlock;
        }

        /* If pid is a child that has execed, return EACCES @@@ This catches
         * spawns _and_ exec's
         */
        if (!(qinfo2.flags & RTMUX_FLG_PROC_FORKED)) {
            ret = EACCES;
            goto out_unlock;
        }

        /* prp1 is the parent so now we can lock him */
        pid1 = ctp->info.pid;
        if (!(prp1 = root_proc_lock(pid1))) {
            ret = EL2HLT;
            goto out_unlock;
        }
    }

    /* session/pgrp moved to root_process_t (POSIX policy out of the kernel
     * process_t). Use the roottask-side slots throughout.
     */
    {
        root_process_t *tmp1 = root_proc(pid1);
        root_process_t *tmp2 = root_proc(pid2);
        root_proc_info_t qinfo2;

        if (process_query_info(pid2, &qinfo2) != EOK) {
            ret = ESRCH;
            goto out_unlock;
        }

        /* If pid is a session leader or not in the same session return EPERM */
        if ((qinfo2.flags & RTMUX_FLG_PROC_SLEADER) ||
            tmp1->session != tmp2->session) {
            ret = EPERM;
            goto out_unlock;
        }

        /* If session is null for either prp1 or prp2 return ESRCH */
        if (!tmp1->session || !tmp2->session) {
            ret = ESRCH;
            goto out_unlock;
        }

        /* if pgid is zero, set process group to indicated pid */
        if (msg->i.pgid == 0) {
            tmp2->pgrp = pid2;
        } else {
            /* If process group is same as calling pid, set process group */
            if (msg->i.pgid != pid1) {
                pid_t leader1, leader3;

                leader1 = tmp1->session->leader;

                if (msg->i.pgid == pid2) {
                    prp3 = prp2;
                    pid3 = pid2;
                    leader3 = root_proc(pid3)->session->leader;
                } else {
                    /* Careful.. unlock prp1/prp2 first */
                    root_proc_unlock(pid1);
                    if (prp2 != prp1)
                        root_proc_unlock(pid2);
                    prp1 = prp2 = NULL;

                    /* If this is not a process group at all, don't set it */
                    pid3 = (pid_t) msg->i.pgid;
                    if (!(prp3 = root_proc_lock(pid3)) ||
                        root_proc(pid3)->pgrp != msg->i.pgid) {
                        ret = EPERM;
                        goto out_unlock;
                    }
                    /* Defensive */
                    if (!root_proc(pid3)->session) {
                        ret = ESRCH;
                        goto out_unlock;
                    }

                    leader3 = root_proc(pid3)->session->leader;
                    root_proc_unlock(pid3);
                    prp3 = NULL;

                    /* Now we need to relock prp2 */
                    if (!(prp2 = root_proc_lock(pid2))) {
                        ret = ESRCH;
                        goto out_unlock;
                    }
                }

                /* If no process group leader is not in same session return
                 * EPERM
                 */
                if (leader1 != leader3) {
                    ret = EPERM;
                    goto out_unlock;
                }
            }
            root_proc(pid2)->pgrp = msg->i.pgid;
        }
    }
out_unlock:
    if (prp3 && prp3 != prp2 && prp3 != prp1)
        root_proc_unlock(pid3);
    if (prp2 && prp2 != prp1)
        root_proc_unlock(pid2);
    return proc_error(ret, pid1);
}
