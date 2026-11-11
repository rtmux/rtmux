/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <private/root-proc-info.h>

/* proc_msg_resource — handle a _PROC_RESOURCE message.
 *
 * Implements getrusage (RUSAGE_SELF / RUSAGE_CHILDREN, sourced from the debug
 * process-info), getrlimit (single resource), and setrlimit (RLIMIT_CPU on the
 * caller only; other resources return ENOSYS).
 *
 * Parameters:
 *   ctp  : I/O service context (ctp->info.pid = calling process)
 *   vmsg : the resource request (subtype selects the operation)
 * Return value: a IOSERV_* status code, or an errno on failure.
 */
int proc_msg_resource(ioserv_context_t *ctp, void *vmsg)
{
    process_t *prp;
    pid_t locked_pid;
    union {
        struct _proc_resource_hdr hdr;
        proc_resource_usage_t usage;
        proc_resource_getlimit_t getlimit;
        proc_resource_setlimit_t setlimit;
    } *msg = vmsg;

    locked_pid = msg->hdr.pid ? (pid_t) msg->hdr.pid : ctp->info.pid;
    if (!(prp = root_proc_lock(locked_pid))) {
        return ESRCH;
    }

    switch (msg->hdr.subtype) {
    case _PROC_RESOURCE_USAGE:
        switch ((int) msg->usage.i.who) {
        case RUSAGE_SELF:
        case RUSAGE_CHILDREN: {
            debug_process_t info;
            uint64_t utimel, stime;
            uint32_t who;

            if (debug_process_call(RTMUX_DEBUG_PROCESS_INFO, locked_pid, 0,
                                   (union rtmux_debug_data *) &info) == -1) {
                return proc_error(errno, locked_pid);
            }
            who = msg->usage.i.who;
            memset(&msg->usage.o, 0x00, sizeof msg->usage.o);

            if (who == RUSAGE_SELF) {
                utimel = info.utime;
                stime = info.stime;
            } else {
                utimel = info.cutime;
                stime = info.cstime;
            }
            utimel /= 1000;
            stime /= 1000;
            msg->usage.o.ru_utime.tv_sec = (time_t) (utimel / 1000000);
            msg->usage.o.ru_utime.tv_usec = (suseconds_t) (utimel % 1000000);
            msg->usage.o.ru_stime.tv_sec = (time_t) (stime / 1000000);
            msg->usage.o.ru_stime.tv_usec = (suseconds_t) (stime % 1000000);
            return proc_error(
                IOSERV_PTR(ctp, &msg->usage.o, sizeof msg->usage.o),
                locked_pid);
        }
        default:
            return proc_error(EINVAL, locked_pid);
        }

    case _PROC_RESOURCE_SETLIMIT:
        /* Only RLIMIT_CPU is settable, and only on the caller itself: the
         * rlimit arrays live kernel-side (heap is PTE_U=0), so the validated
         * pair goes through ProcessSetRlimitCpu. A process may lower either
         * limit but not raise the hard limit (no privilege override yet).
         */
        if (msg->hdr.pid) {
            return proc_error(EPERM, locked_pid);
        }
        if (msg->setlimit.i.count != 1 ||
            msg->setlimit.i.entry[0].resource >= RLIM_NLIMITS) {
            return proc_error(EINVAL, locked_pid);
        }
        if (msg->setlimit.i.entry[0].resource != RLIMIT_CPU) {
            return proc_error(ENOSYS, locked_pid);
        }
        {
            root_proc_info_t qinfo_rl;
            struct rlimit nl = msg->setlimit.i.entry[0].limit;

            if (process_query_info(locked_pid, &qinfo_rl) != EOK) {
                return proc_error(ESRCH, locked_pid);
            }
            if (nl.rlim_cur > nl.rlim_max) {
                return proc_error(EINVAL, locked_pid);
            }
            if (nl.rlim_max > qinfo_rl.rlimit_vals_hard[RLIMIT_CPU]) {
                return proc_error(EPERM, locked_pid);
            }
            return proc_error(
                ProcessSetRlimitCpu(locked_pid, nl.rlim_cur, nl.rlim_max),
                locked_pid);
        }

    case _PROC_RESOURCE_GETLIMIT:
        if (msg->getlimit.i.count != 1) {
            return proc_error(ENOTSUP, locked_pid);
        }
        {
            root_proc_info_t qinfo_rl;
            if (process_query_info(locked_pid, &qinfo_rl) != EOK) {
                return proc_error(ESRCH, locked_pid);
            }
            switch (msg->getlimit.i.resource[0]) {
            case RLIMIT_CORE:
            case RLIMIT_NOFILE:
            case RLIMIT_AS:
            case RLIMIT_STACK:
            case RLIMIT_DATA:
            case RLIMIT_NTHR:
            case RLIMIT_NPROC:
            case RLIMIT_CPU: {
                unsigned long s =
                    qinfo_rl.rlimit_vals_soft[msg->getlimit.i.resource[0]];
                unsigned long h =
                    qinfo_rl.rlimit_vals_hard[msg->getlimit.i.resource[0]];
                msg->getlimit.o[0].rlim_cur =
                    (s == RLIM_INFINITY) ? RLIM_INFINITY : s;
                msg->getlimit.o[0].rlim_max =
                    (h == RLIM_INFINITY) ? RLIM_INFINITY : h;
                break;
            }
            case RLIMIT_FSIZE:
            case RLIMIT_MEMLOCK:
                msg->getlimit.o[0].rlim_cur =
                    RLIM_INFINITY; /* Limits ignored */
                msg->getlimit.o[0].rlim_max =
                    RLIM_INFINITY; /* Limits ignored */
                break;
            default:
                return proc_error(EINVAL, locked_pid);
            }
        }
        return proc_error(
            IOSERV_PTR(ctp, &msg->getlimit.o, sizeof msg->getlimit.o),
            locked_pid);

    default:
        return proc_error(ENOSYS, locked_pid);
    }

    return proc_error(EOK, locked_pid);
}
