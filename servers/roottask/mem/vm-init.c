/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mm-internal.h"
#include "as.h"
#include "root-log.h"
#include "root-process.h"
#include <private/root-proc-info.h>

/* Address-space operation table. */
static vm_ops_t as = {.pagesize = __PAGESIZE,
                      .sizeof_address_entry = sizeof(address_t),

#undef MMF
#define MMF(r, f, p, e) .f = e##_##f,
                      AS_FUNCS(as)};

/* Handle an incoming VM message.
 *
 * Dispatches _MEM_MAP, _MEM_CTRL, _MEM_INFO, _MEM_OFFSET, _MEM_DEBUG_INFO, and
 * _MEM_SWAP for the sending process, or for a peer process named via a leading
 * _MEM_PEER wrapper. Locks the target process's address space appropriately
 * before calling the matching vm_* routine. Parameters:
 *   mctp  : message context (cast to ioserv_context_t)
 *   code  : message code (unused)
 *   flags : dispatch flags (unused)
 *   handle: dispatch handle (unused)
 * Return value: result of proc_status() for the reply.
 */
static int vm_handler(message_context_t *mctp,
                      int code,
                      unsigned flags,
                      void *handle)
{
    ioserv_context_t *ctp = (ioserv_context_t *) mctp;
    union {
        uint16_t type;
        mem_map_t map;
        mem_ctrl_t ctrl;
        mem_info_t info;
        mem_offset_t offset;
        mem_debug_info_t debug_info;
        mem_swap_t swap;
        mem_peer_t peer;
        mem_handle_t mem_handle;
    } *msg = (void *) ctp->msg;
    process_t *prp;
    process_t *peer;
    pid_t prp_pid;
    pid_t peer_pid = 0;

    /* The kernel heap is PTE_U=0, so roottask cannot chase prp->cred /
     * peer->cred kernel-VA pointer chains. The credential snapshot is pulled
     * from process_query_info (qinfo.cred is a "struct _cred_info" value,
     * copied out by the kernel extension).
     */
    root_proc_info_t prp_info, peer_info;
    int status;

    /* We are reply-blocked on proc, so prp cannot go away */
    prp_pid = ctp->info.pid;
    if (!(prp = root_proc_lookup(prp_pid))) {
        return proc_status(ctp, EL2HLT);
    }

    ctp->status = 0;

peer_restart:
    process_bind(prp_pid);
    switch (msg->type) {
    case _MEM_PEER:
        process_bind(0);
        if (ctp->info.msglen <= msg->peer.i.peer_msg_len) {
            /* no message following */
            return proc_status(ctp, EINVAL);
        }
        /* Have to lock the peer, since it might disappear on us otherwise */
        if (msg->peer.i.pid == ctp->info.pid) {
            peer = prp;
            peer_pid = prp_pid;
        } else {
            peer_pid = (pid_t) msg->peer.i.pid;
            if (!(peer = root_proc_lock(peer_pid))) {
                return proc_status(ctp, ESRCH);
            }
        }
        if (process_query_info(peer_pid, &peer_info) != EOK ||
            (peer_info.flags &
             (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL |
              RTMUX_FLG_PROC_ZOMBIE))) {
            /* Not allowed to do things with a peer process that's terminating.
             */
            if (peer != prp)
                root_proc_unlock(peer_pid);
            return proc_status(ctp, ESRCH);
        }
        /* You might be tempted to just adjust the msg pointer to after the peer
         * message. The problem is that the various vm_* routines use msg as
         * both an input and output area. If we just add to 'msg', the reply
         * message being built will go in the wrong spot, since the
         * proc_status() call down at the bottom of this routine msg_reply's
         * from the start of the message buffer.
         *
         * Update msglen to the inner length too: after the memmove the received
         * message IS the inner one, and callees floor-check their variable
         * payload against ctp->info.msglen (msglen > peer_msg_len is guaranteed
         * by the _MEM_PEER length check above).
         */
        {
            uint32_t inner_len = ctp->info.msglen - msg->peer.i.peer_msg_len;

            memmove(msg, (uint8_t *) msg + msg->peer.i.peer_msg_len, inner_len);
            ctp->info.msglen = inner_len;
        }
        switch (msg->type) {
        case _MEM_MAP:
        case _MEM_CTRL:
            if (process_query_info(prp_pid, &prp_info) != EOK) {
                if (peer != prp)
                    root_proc_unlock(peer_pid);
                return proc_status(ctp, ESRCH);
            }
            if (!(prp_info.cred.euid == 0 ||
                  prp_info.cred.ruid == peer_info.cred.ruid ||
                  prp_info.cred.ruid == peer_info.cred.suid ||
                  prp_info.cred.euid == peer_info.cred.ruid ||
                  prp_info.cred.euid == peer_info.cred.suid)) {
                /* Only root and processes with the same userid are allowed to
                 * manipulate another process.
                 */
                if (peer != prp)
                    root_proc_unlock(peer_pid);
                return proc_status(ctp, EPERM);
            }
            break;
        case _MEM_INFO:
        case _MEM_OFFSET:
        case _MEM_DEBUG_INFO:
            /* always OK to look at another process */
            break;
        default:
            if (peer != prp)
                root_proc_unlock(peer_pid);
            return proc_status(ctp, EINVAL);
        }
        /* Try again, with the peer process */
        prp = peer;
        prp_pid = peer_pid;
        goto peer_restart;

    case _MEM_MAP:
        status = proc_wlock_adp(prp_pid);
        CRASHCHECK(status == -1);
        status = vm_map(ctp, prp_pid, prp, &msg->map);
        break;

    case _MEM_CTRL:
        status = proc_wlock_adp(prp_pid);
        CRASHCHECK(status == -1);
        status = vm_ctrl(prp_pid, prp, &msg->ctrl);
        break;

    case _MEM_INFO:
        status = proc_rlock_adp(prp_pid);
        CRASHCHECK(status == -1);
        status = vm_info(ctp, prp_pid, prp, &msg->info);
        break;

    case _MEM_HANDLE:
        /* Serialize per-process cap-table state the way the other _MEM_* ops
         * serialize the address space. Without it two threads racing the first
         * handle op both see tmp->mem_cap_slots unset in memcap_ensure(),
         * double-init tmp->mem_caps, and leak/corrupt one slot array.
         * _MEM_HANDLE is only reachable for the caller's own pid (prp_pid ==
         * ctp->info.pid), so no peer unlock is needed. vm_handle still runs
         * unbound.
         */
        status = proc_wlock_adp(prp_pid);
        CRASHCHECK(status == -1);
        process_bind(0);
        status = vm_handle(ctp, prp_pid, prp, &msg->mem_handle);
        proc_unlock_adp(prp_pid);
        return proc_status(ctp, status);

    case _MEM_OFFSET:
        status = proc_rlock_adp(prp_pid);
        CRASHCHECK(status == -1);
        status = vm_offset(ctp, prp_pid, prp, &msg->offset);
        break;

    case _MEM_DEBUG_INFO:
        status = proc_rlock_adp(prp_pid);
        CRASHCHECK(status == -1);
        status = vm_debug_info(ctp, prp_pid, prp, &msg->debug_info);
        break;

    case _MEM_SWAP:
        /* No swap device in RTMux */
        process_bind(0);
        return proc_status(ctp, ENOSYS);

    default:
        process_bind(0);
        return proc_status(ctp, ENOSYS);
    }

    process_bind(0);

    proc_unlock_adp(prp_pid);

    if (prp_pid != ctp->info.pid) {
        /* We're doing a peer process manipulation */
        root_proc_unlock(prp_pid);
    }

    return proc_status(ctp, status);
}

/* Initialize the roottask VM service.
 *
 * \note address_t is allocated separately in as_mcreate() rather than
 *       embedded inside process objects. The embedded pattern requires kernel
 *       registration before any process allocation, which is not possible in
 *       RTMux where the kernel and roottask are separate binaries.
 */
void vm_init(void)
{
    /* Phase 0: initialize physical allocator, page tables, etc. This sets
     * restrict_user (needed for init_vm_ops to switch vtable).
     */
    as_init_mem(0);

    if (restrict_user) {
        /* Switch to as */
        if (!(SYSPAGE_ENTRY(cpuinfo)->flags & CPU_FLAG_MMU))
            crash();
        root_dbg("%s: switching to as\n", __func__);
        root_vm_ops = as;

        /* The address space for the system process (pid 1) is set up by the
         * kernel before roottask runs (roottask.c allocates the address_t and
         * points adp->cpu.pgdir at the U-mode roottask pgdir). The per-pid
         * roottask state (mm_map_head, anon, rlimit) lives in root_process_t
         * and must be initialized here so that roottask's own root_vm_ops.mmap
         * calls (devmem.c, procfs.c, …) on ROOTTASK_PID find a valid map range.
         */
        {
            int r = as_mcreate_pid(ROOTTASK_PID);
            if (r != EOK)
                crash();
        }
    }

    devzero_init();
    devmem_init();
    vm_fd_init();

    /* Redirect _VM_* messages to the VM handler. */
    message_attach(dpp, NULL, _VM_BASE, _VM_MAX, vm_handler, NULL);

    /* Initialize any VM background processing. */
    root_vm_ops.init_mem(1);
}


/* Resize a memory object's backing store.
 *
 * Rejects objects flagged MM_SHMEM_SPECIAL, which cannot be resized; otherwise
 * delegates to the active root_vm_ops's resize routine. Parameters:
 *   obj  : memory object to resize
 *   size : new size in bytes
 * Return value: EOK on success, or an errno on failure.
 */
int vm_resize(path_object_t *obj, size_t size)
{
    if (obj->mem.mm.flags & MM_SHMEM_SPECIAL) {
        return EINVAL;
    }
    return root_vm_ops.resize(obj, size);
}
