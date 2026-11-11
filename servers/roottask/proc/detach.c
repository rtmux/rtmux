/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* proc_detach — detach a process from its parent ("stay resident").
 *
 * The calling process's parent is notified as if the child exited (unblocking
 * waitpid), and the process is reparented to pid 1. The process continues
 * running — its threads keep servicing channels and dispatch loops.
 *
 * License: Apache 2.0
 */

#include "externs.h"
#include "private.h"
#include "root-process.h"
#include <private/root-proc-info.h>

/* proc_detach — handle _PROC_DETACH message.
 *
 * @ctp : I/O service context (ctp->info.pid = calling process)
 * @msg : message with status field
 *
 * Return EOK on success, errno on failure
 */
int proc_detach(ioserv_context_t *ctp, proc_detach_t *msg)
{
    process_t *prp;
    root_proc_info_t qinfo;
    pid_t pid;

    pid = ctp->info.pid;
    if (!(prp = root_proc_lock(pid)))
        return ESRCH;

    /* prp is in kernel heap (PTE_U=0); read parent_pid via the
     * process_query_info snapshot.
     */
    if (process_query_info(pid, &qinfo) != EOK) {
        root_proc_unlock(pid);
        return ESRCH;
    }

    /* Already detached? */
    if (qinfo.parent_pid == ROOTTASK_PID) {
        root_proc_unlock(pid);
        return EOK;
    }

    /* Deliver status to the parent's waitpid without holding the child lock.
     * proc_nozombie() locks the parent wait list; holding child here can
     * deadlock against a parent thread concurrently polling waitpid(WNOHANG).
     */
    root_proc_unlock(pid);
    proc_nozombie(pid, msg->i.status);

    /* Reparent to pid 1 (priv body lives in kernel/ext/reparent.c). */
    if (!(prp = root_proc_lock(pid)))
        return ESRCH;
    process_reparent(prp);

    root_proc_unlock(pid);
    return EOK;
}

/* proc_phys_contain — handle _PROC_PHYS_CONTAIN.
 *
 * The calling process self-restricts its address space: every subsequent
 * specific-address MAP_PHYS request is refused (enforced in vm-map.c; MAP_ANON
 * DMA buffers stay allowed). The flag is monotonic — once set it never clears —
 * so a process can only tighten, never loosen, its own physical-mapping rights.
 *
 * @ctp : I/O service context (ctp->info.pid = calling process)
 * @msg : _PROC_PHYS_CONTAIN message (no payload)
 *
 * Return EOK on success, ESRCH if the caller slot is gone
 */
int proc_phys_contain(ioserv_context_t *ctp, proc_phys_contain_t *msg)
{
    pid_t pid = ctp->info.pid;

    (void) msg;

    /* Set the flag under the address-space write lock — the same lock _MEM_MAP
     * holds across vm_map — so it serializes with any in-flight mmap and every
     * later mmap observes the sealed state. The caller is blocked in this send,
     * so it cannot be mapping concurrently and there is no self-deadlock.
     */
    (void) proc_wlock_adp(pid);
    root_proc(pid)->phys_contained = true;
    proc_unlock_adp(pid);
    return EOK;
}
