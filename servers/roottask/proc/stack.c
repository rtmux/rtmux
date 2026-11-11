/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "private.h"

/* proc_stack — thread-stack allocation notification handler.
 *
 * Fetches the pending stack request (stack_wait_info), then either maps a new
 * thread stack (with guard region) into the target process or unmaps an
 * existing one, and resumes the requesting thread with the result (stack_cont).
 *
 * Parameters:
 *   ctp    : message context (notification value identifies the request)
 *   code   : notification code (unused)
 *   flags  : dispatch flags (unused)
 *   handle : dispatch handle (unused)
 * Return value: 0.
 */
int proc_stack(message_context_t *ctp, int code, unsigned flags, void *handle)
{
    union sigval value = ctp->msg->notification.value;
    process_t *prp;
    pid_t pid;
    int tid;
    struct _thread_attr attr;
    int status;


    if (stack_wait_info(value, &pid, &tid, &attr) == -1) {
        return 0;
    }
    status = EL2HLT;
    if ((prp = root_proc_lookup(pid))) {
        if (proc_wlock_adp(pid) != 0) {
            crash();
        }
        process_bind(pid); /* prp->pid was kernel-VA deref; pid is in scope */

        if (attr.guardsize == ~0U) {
            (void) root_vm_ops.munmap(pid, prp, (uintptr_t) attr.stackaddr,
                                      attr.stacksize, 0);
            status = EOK;
        } else {
            unsigned mmap_flags;

            mmap_flags = MAP_PRIVATE | MAP_ANON | MAP_STACK | MAP_NOINIT;
            status =
                root_vm_ops.mmap(pid, prp, 0, attr.stacksize + attr.guardsize,
                                 PROT_READ | PROT_WRITE, (int) mmap_flags, 0,
                                 attr.guardsize, __PAGESIZE, attr.prealloc,
                                 NOFD, &attr.stackaddr, &attr.stacksize);
            if (status == ENOMEM) {
                status = EAGAIN;
            }
        }

        process_bind(0);
        proc_unlock_adp(pid);
    }

    (void) stack_cont(pid, tid, status, &attr);
    return 0;
}
