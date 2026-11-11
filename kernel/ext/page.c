/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/memmsg.h>

struct kcall_args_page_wait_info {
    unsigned value;
    pid_t *ppid;
    int *ptid;
    pid_t *as_pidp;
    unsigned *pflags;
    uintptr_t vaddr;
};

/* Retrieve a page-fault-waiting thread's saved fault info.
 *
 * Decodes the (process-index, tid) pair from the sync-owner value, and if the
 * matching thread is in STATE_WAITPAGE, copies out its faulting vaddr, pid,
 * tid, flags, and as pid. Sets vaddr to ~0 if no such thread is found.
 * Parameters:
 *   data : pointer to a struct kcall_args_page_wait_info (in: value/vaddr;
 *          out: ppid, ptid, as_pidp, pflags, vaddr)
 */
static void page_wait_info(void *data)
{
    struct kcall_args_page_wait_info *kap = data;
    int id;
    unsigned index;
    pid_t pid;
    process_t *prp;
    thread_t *thp;
    unsigned pt_info;

    pt_info = kap->value;
    id = SYNC_PINDEX(pt_info);
    if (id < process_vector.nentries &&
        (prp = VECP(prp, &process_vector, id)) && id == PINDEX(prp->pid) &&
        (thp = vector_lookup(&prp->threads, SYNC_TID(pt_info))) &&
        (thp->state == STATE_WAITPAGE)) {
        kap->vaddr = (uintptr_t) thp->next.thread;
        *kap->ppid = prp->pid;
        *kap->ptid = (int) (thp->tid + 1);
        *kap->pflags = (unsigned) (uintptr_t) thp->status;

        pid = (pid_t) (intptr_t) thp->prev.thread;
        index = PINDEX(pid);
        if ((index >= process_vector.nentries) ||
            !(prp = VECP(prp, &process_vector, index)) || (prp->pid != pid)) {
            pid = 0;
        }
        *kap->as_pidp = pid;
    } else {
        kap->vaddr = ~(uintptr_t) 0;
    }
}

/* Fill a debug_thread_t with a thread's page-wait blocking details.
 *
 * Builds the sync-owner value from the thread's stashed fault info and runs
 * page_wait_info to populate the dtp->blocked.waitpage fields (pid, flags,
 * vaddr) for the debugger. Parameters:
 *   thp : the page-fault-waiting thread to describe
 *   dtp : debug_thread_t whose blocked.waitpage union is filled in
 */
void waitpage_status_get(thread_t *thp, debug_thread_t *dtp)
{
    struct kcall_args_page_wait_info ka;
    pid_t aspid; /* place holder */
    int tid;     /* place holder */

    ka.value =
        SYNC_OWNER_BITS((unsigned) (uintptr_t) thp->prev.thread, thp->tid);
    ka.ppid = &dtp->blocked.waitpage.pid;
    ka.ptid = &tid;
    ka.as_pidp = &aspid;
    ka.pflags = &dtp->blocked.waitpage.flags;

    dtp->blocked.waitpage.pid = 0; /* Initialize */
    page_wait_info(&ka);

    dtp->blocked.waitpage.vaddr = ka.vaddr;

    return;
}
