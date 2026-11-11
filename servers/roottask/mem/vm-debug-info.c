/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/trace.h>
#include "externs.h"

int vm_debug_info(ioserv_context_t *ctp,
                  pid_t prp_pid,
                  process_t *prp,
                  mem_debug_info_t *msg)
{
    int ret;

    /* The client controls the flexible path[] tail. Reject a message whose
     * received length does not cover the header, or whose path is not
     * NUL-terminated within the received window, before debuginfo() strdup's
     * the name (and before the instrumentation strlen below) reads off the end.
     */
    if (ctp->info.msglen <= offsetof(struct _mem_debug_info, path) ||
        memchr(msg->i.path, '\0',
               (size_t) ctp->info.msglen -
                   offsetof(struct _mem_debug_info, path)) == NULL)
        return EINVAL;

    ret = root_vm_ops.debuginfo(prp_pid, prp, &msg->i);

#if defined(CONFIG_INSTRUMENTATION)
    if (ret == EOK) {
        iov_t iov[4];

        /* Use the prp_pid function argument, NOT &prp->pid: process_t's body is
         * gated behind KERNEL_INTERNAL and is an opaque typedef in roottask.
         * prp_pid is the same value the kernel would publish via prp->pid
         * (roottask's process_create wrapper passes the kernel-allocated pid
         * back), so the trace event payload is unchanged.
         */
        SETIOV(iov, &prp_pid, sizeof(prp_pid));
        SETIOV(iov + 1, &msg->i.vaddr, sizeof(msg->i.vaddr));
        SETIOV(iov + 2, &msg->i.size, sizeof(msg->i.size));
        SETIOV(iov + 3, msg->i.path, strlen(msg->i.path));
        (void) kext_add_trace_event_iov(_TRACE_SYSTEM_C,
                                        RTMUX_TRACE_SYS_MAPNAME, iov, 4);
    }
#endif

    return ret;
}
