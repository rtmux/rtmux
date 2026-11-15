/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"
#include <stdint.h>
#include <sys/resource.h>
#include <sys/rtmux-core.h>

/* rlimit_blown — non-zero if growing pid's usage of resource `which` from `cur`
 * by `add` bytes would exceed its soft limit.
 *
 * The soft limits live in the kernel-allocated process_t (heap is PTE_U=0), so
 * they come from a process_query_info snapshot. Callers that already hold one
 * pass it as `info`; NULL takes a fresh one. The check fails closed: a failed
 * query, an unknown resource, or a sum that overflows counts as over the limit.
 * pid 0 (system mappings) and roottask itself are exempt, since they map memory
 * during boot before any limit could apply and must never be refused by one.
 */
int rdecl rlimit_blown(pid_t pid,
                       const struct root_proc_info *info,
                       int which,
                       size_t cur,
                       size_t add)
{
    root_proc_info_t snap;
    unsigned long lim;

    if (pid == 0 || pid == ROOTTASK_PID)
        return 0;
    if (add > SIZE_MAX - cur)
        return 1;
    if (which < 0 || which >= RLIM_NLIMITS)
        return 1;
    if (!info) {
        if (process_query_info(pid, &snap) != EOK)
            return 1;
        info = &snap;
    }
    lim = info->rlimit_vals_soft[which];
    return lim != RLIM_INFINITY && cur + add > lim;
}
