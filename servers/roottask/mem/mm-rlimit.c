/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"
#include <sys/resource.h>

int rdecl rlimit_blown(pid_t pid, int which, size_t new)
{
    /* prp->rlimit_vals_soft used to be a direct deref of the kernel- allocated
     * process_t. In U-mode roottask, that's a PTE_U=0 load fault. Route through
     * process_query_info + a follow-up kernel extension when we need the
     * per-RLIMIT soft value. For the boot path we accept RLIM_INFINITY ("no
     * limit") if the snapshot fails.
     */
    root_proc_info_t info;
    (void) which;
    (void) new;
    if (pid == 0)
        return 0;
    if (process_query_info(pid, &info) != EOK)
        return 0;

    /* TODO: rlimit_vals_soft snapshot — for now treat all checks as pass.
     * Revisit when we wire a per-rlimit kext call.
     */
    return 0;
}
