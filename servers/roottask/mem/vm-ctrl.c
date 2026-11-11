/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/memmsg.h>
#include <sys/trace.h>
#include "mm-internal.h"

/* This flag determines whether we should enfore address alignement restrictions
 * as documented by POSIX 1003.1. This is only done if roottask was run with the
 * -ma flag.
 */
#define VV_ALIGN 0x1000
#define VM_MADV_MAX 4

static int validate_vaddr(pid_t prp_pid,
                          process_t *prp,
                          uintptr_t vaddr,
                          size_t len,
                          int flags)
{
    /* Was: "if (prp->flags & RTMUX_FLG_PROC_TERMING)" short-circuit. Removing
     * that path: in U-mode roottask we can't deref prp->flags here. Falling
     * through to root_vm_ops.validate is correct — that function takes pid and
     * looks up flags via the kernel side if needed.
     */
    if ((flags & VV_ALIGN) && (mm_flags & MM_FLAG_ENFORCE_ALIGNMENT)) {
        if ((vaddr & (root_vm_ops.pagesize - 1)) != 0)
            return EINVAL;
    }
    return root_vm_ops.validate(prp_pid, prp, vaddr, len, flags);
}


int vm_ctrl(pid_t prp_pid, process_t *prp, mem_ctrl_t *msg)
{
    int r;
    int flags;
    unsigned mflags = msg->i.flags;

    /* If the range is too large, quietly truncate it All the casting is to do
     * 32-bit arithmetic on 32-bit machines
     */

    if ((size_t) msg->i.len > ~(uintptr_t) 0 - (uintptr_t) msg->i.addr) {
        msg->i.len = ~(uintptr_t) 0 - (uintptr_t) msg->i.addr;
    }

    switch (msg->i.subtype) {
    case _MEM_CTRL_UNMAP:
        if (msg->i.len == 0) {
            return EINVAL;
        }
        if (msg->i.flags &
            ~(uint32_t) (MM_UNMAP_INIT_REQUIRED | MM_UNMAP_INIT_OPTIONAL)) {
            return EINVAL;
        }
        r = validate_vaddr(prp_pid, prp, msg->i.addr, msg->i.len,
                           VV_ALIGN | VV_RANGE);
        if (r != EOK)
            return r;
        r = root_vm_ops.munmap(prp_pid, prp, msg->i.addr, msg->i.len,
                               (int) mflags);

#if defined(CONFIG_INSTRUMENTATION)
        {
            iov_t iov[2];
            SETIOV(iov, &prp_pid, sizeof(prp_pid));
            SETIOV(iov + 1, &msg->i.addr,
                   sizeof(msg->i.addr) + sizeof(msg->i.len));
            (void) kext_add_trace_event_iov(_TRACE_SYSTEM_C,
                                            RTMUX_TRACE_SYS_MUNMAP, iov, 2);
        }
#endif

        return r;

    case _MEM_CTRL_PROTECT:
        if (msg->i.flags & ~(uint32_t) PROT_MASK) {
            return EINVAL;
        }

        flags = VV_RANGE | VV_MAPPED;
        /* Alignment is enforced in the pager for MAP_ELF mappings. */
        r = validate_vaddr(prp_pid, prp, msg->i.addr, msg->i.len, flags);
        if (r != EOK)
            return r;
        return root_vm_ops.mprotect(prp_pid, prp, msg->i.addr, msg->i.len,
                                    (int) mflags);

    case _MEM_CTRL_SYNC:
#define ALL \
    (MS_INVALIDATE_ICACHE | MS_CACHE_ONLY | MS_SYNC | MS_ASYNC | MS_INVALIDATE)
        if (msg->i.flags & ~(uint32_t) ALL) {
            /* Illegal bit turned on */
            return EINVAL;
        }
        if ((msg->i.flags & (MS_SYNC | MS_ASYNC)) == (MS_SYNC | MS_ASYNC)) {
            /* Not allowed to turn both on */
            return EINVAL;
        }
        flags = VV_RANGE | VV_MAPPED;
        if (!(msg->i.flags & (MS_INVALIDATE_ICACHE | MS_CACHE_ONLY))) {
            /* Non-cache operations may need to be page aligned */
            flags |= VV_ALIGN;
        }
        r = validate_vaddr(prp_pid, prp, msg->i.addr, msg->i.len, flags);
        if (r != EOK)
            return r;
        return root_vm_ops.msync(prp_pid, prp, msg->i.addr, msg->i.len,
                                 (int) mflags);

    /* mlock/munlock removed — RTMux has no demand paging */
    case _MEM_CTRL_LOCKALL:
    case _MEM_CTRL_LOCK:
    case _MEM_CTRL_UNLOCKALL:
    case _MEM_CTRL_UNLOCK:
        return EOK;

    case _MEM_CTRL_ADVISE:
        if (msg->i.flags > VM_MADV_MAX) {
            /* Don't need to check for "< 0" because flags is unsigned */
            return EINVAL;
        }
        r = validate_vaddr(prp_pid, prp, msg->i.addr, msg->i.len,
                           VV_RANGE | VV_MAPPED);
        if (r != EOK)
            return r;
        return root_vm_ops.madvise(prp_pid, prp, msg->i.addr, msg->i.len,
                                   (int) mflags);

    default:
        break;
    }

    return ENOSYS;
}
