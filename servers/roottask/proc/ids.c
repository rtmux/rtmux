/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"

/* session_id — the session ID a snapshot reports: its session leader's pid. A
 * process that never joined a session (no ancestor called setsid) belongs to
 * the system session, which roottask leads, as getppid() reports roottask for a
 * process with no parent.
 */
static pid_t session_id(const root_proc_info_t *qinfo)
{
    return qinfo->session ? qinfo->sess_leader : ROOTTASK_PID;
}

/* proc_getsetid — handle a _PROC_GETSETID message.
 *
 * Implements getid (pgrp / ppid / sid / cred) plus set*id / set*gid variants,
 * enforcing the POSIX permission rules (only root may change another process's
 * ids or relax the saved id).
 *
 * Parameters:
 *   ctp : I/O service context (ctp->info.pid = calling process)
 *   msg : the get/set-id request (subtype + target pid + values)
 * Return value: EOK / an errno, or a IOSERV_* status code.
 */
int proc_getsetid(ioserv_context_t *ctp, proc_getsetid_t *msg)
{
    process_t *prp;
    pid_t locked_pid;
    int isroot;
    int *rid, *eid, *sid;
    struct _cred_info info;
    root_proc_info_t qinfo; /* snapshot of prp's kernel-half fields */

    locked_pid = msg->i.pid ? (pid_t) msg->i.pid : ctp->info.pid;
    if (!(prp = root_proc_lock(locked_pid))) {
        return ESRCH;
    }

    /* Pre-PTE_U=0 this code dereferenced prp directly (prp->pid, prp->pgrp,
     * prp->parent->pid, prp->session->leader). prp lives in the kernel heap
     * (PTE_U=0); U-mode roottask cannot read it. Snapshot the fields once via
     * process_query_info and use the snapshot for the rest of the function. A
     * reaped child (WAITDONE) lingers until its teardown finishes, but its pid
     * no longer names a process. A zombie still does.
     */
    if (process_query_info(locked_pid, &qinfo) != EOK ||
        (qinfo.flags & RTMUX_FLG_PROC_WAITDONE)) {
        root_proc_unlock(locked_pid);
        return ESRCH;
    }

    (void) cred_get_call(locked_pid, &info);

    switch (msg->i.subtype) {
    case _PROC_ID_GETID: {
        pid_t sess = session_id(&qinfo);

        /* The reply overlays the request (o.pgrp shares i.pid's bytes), so
         * locked_pid, taken above, is the only copy of the target from here.
         */
        msg->o.pgrp = qinfo.pgrp;
        msg->o.ppid = qinfo.parent_pid ? qinfo.parent_pid : ROOTTASK_PID;
        msg->o.sid = sess;
        msg->o.cred = info;
        SETIOV(ctp->iov + 0, &msg->o,
               offsetof(struct _proc_getsetid_reply, cred.grouplist) +
                   msg->o.cred.ngroups * sizeof info.grouplist[0]);
        /* Unlock the target before locking the caller: never hold two. */
        root_proc_unlock(locked_pid);

        /* POSIX lets getsid() refuse a process in another session (EPERM). */
        if (locked_pid != ctp->info.pid) {
            root_proc_info_t self;

            if (!root_proc_lock(ctp->info.pid))
                return ESRCH;
            if (process_query_info(ctp->info.pid, &self) != EOK ||
                session_id(&self) != sess)
                return proc_error(EPERM, ctp->info.pid);
            root_proc_unlock(ctp->info.pid);
        }

        return IOSERV_NPARTS(1);
    }

    case _PROC_ID_SETGROUPS:
        if (!proc_isaccess(0, &ctp->info)) {
            /* Only allow root to change group lists */
            return proc_error(EPERM, locked_pid);
        }

        if (msg->i.ngroups < 0 ||
            msg->i.ngroups > sizeof info.grouplist / sizeof info.grouplist[0]) {
            return proc_error(EINVAL, locked_pid);
        }

        /* The grouplist tail follows the header; reject a message too short to
         * hold the declared ngroups entries before copying them.
         */
        if (ctp->info.msglen < sizeof(msg->i) + (size_t) msg->i.ngroups *
                                                    sizeof info.grouplist[0]) {
            return proc_error(EINVAL, locked_pid);
        }
        /* Get the current credential and update the grouplist. */
        info.ngroups = (uint32_t) msg->i.ngroups;
        memcpy(info.grouplist, (&msg->i + 1),
               (size_t) msg->i.ngroups * sizeof info.grouplist[0]);
        cred_set_call(locked_pid, &info);
        break;

    case _PROC_ID_SETUID:
    case _PROC_ID_SETEUID:
    case _PROC_ID_SETREUID:
        /* Changing user parameters */
        rid = &info.ruid;
        eid = &info.euid;
        sid = &info.suid;
        goto setid1;

    case _PROC_ID_SETGID:
    case _PROC_ID_SETEGID:
    case _PROC_ID_SETREGID:
        /* Changing group parameters */
        rid = &info.rgid;
        eid = &info.egid;
        sid = &info.sgid;

    setid1:
        isroot = proc_isaccess(0, &ctp->info);
        if (!isroot && locked_pid != ctp->info.pid) {
            /* Only allow root to change someone elses perms */
            return proc_error(EPERM, locked_pid);
        }

        switch (msg->i.subtype) {
        case _PROC_ID_SETUID:
        case _PROC_ID_SETGID:
            /* set?id acts like sete?id if not root */
            if (isroot) {
                msg->i.rid = msg->i.eid;
                goto setid2;
            }
            /* Fall through */

        case _PROC_ID_SETEUID:
        case _PROC_ID_SETEGID:
            /* sete?id doesn't change the real id */
            msg->i.rid = -1;

            if (isroot) {
                /* If root, don't allow change saved-id */
                sid = 0;
            }
            /* Fall through */

        case _PROC_ID_SETREUID:
        case _PROC_ID_SETREGID:
            if (!isroot) {
                /* If not root and trying to change eid, it must be equal to rid
                 * or sid
                 */
                if (msg->i.eid != -1 && msg->i.eid != *rid &&
                    msg->i.eid != *sid) {
                    return proc_error(EPERM, locked_pid);
                }

                /* If not root and trying to change rid, it must be equal to eid
                 */
                if (msg->i.rid != -1 && msg->i.rid != *eid) {
                    return proc_error(EPERM, locked_pid);
                }
            }

        setid2:
            /* change the effective id */
            if (msg->i.eid != -1) {
                *eid = msg->i.eid;
            }
            /* change the real id */
            if (msg->i.rid != -1) {
                *rid = msg->i.rid;
            }

            /* If changeing real, or changing effective to other that read, set
             * saved to effective
             */
            if (sid && (msg->i.rid != -1 ||
                        (msg->i.eid != -1 && msg->i.eid != *rid))) {
                *sid = *eid;
            }
            cred_set_call(locked_pid, &info);
            break;
        default:
            break;
        }
        break;

    default:
        return proc_error(ENOSYS, locked_pid);
    }

    return proc_error(EOK, locked_pid);
}
