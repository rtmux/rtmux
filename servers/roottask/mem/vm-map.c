/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/mman.h>
#include <sys/trace.h>
#include <hwres-types.h>
#include "mm-internal.h"
#include "root-process.h"

#define VM_MAP_HWRES_COUNT 16

static int (*const fs_check[])(const ioserv_ops_t *,
                               mem_map_t *,
                               void *,
                               path_object_t **) = {devmem_check,
                                                    devzero_check};

static int phys_range_in_hwres(pid_t pid,
                               paddr64_t start,
                               paddr64_t end,
                               int type)
{
    hwres_range_t list[VM_MAP_HWRES_COUNT];
    int idx = 0;

    for (;;) {
        int num = hwres_proc_query_pid(pid, list, NUM_ELTS(list), idx, type);

        if (num <= 0)
            return 0;
        idx += num;
        for (int i = 0; i < num; i++) {
            if (list[i].start <= start && end <= list[i].end)
                return 1;
        }
    }
}

static int contained_phys_map_ok(pid_t pid, mem_map_t *msg)
{
    paddr64_t start;
    paddr64_t end;

    if (!root_proc(pid)->phys_contained || !(msg->i.flags & MAP_PHYS) ||
        (msg->i.flags & MAP_ANON))
        return EOK;

    start = (paddr64_t) msg->i.offset;
    if (msg->i.len == 0 || UINT64_MAX - start < msg->i.len)
        return EINVAL;
    end = ROUNDUP(start + msg->i.len, __PAGESIZE) - 1;
    if (end < start)
        return EINVAL;
    if (phys_range_in_hwres(pid, start, end, HWRES_MEMORY) ||
        phys_range_in_hwres(pid, start, end, HWRES_PCI_MEMORY))
        return EOK;
    return EPERM;
}

/* Find the memory object backing an mmap fd.
 *
 * First offers the fd to the filesystem-specific checkers (devmem, devzero). If
 * none claims it, falls through to vm_open_fd for generic file mapping.
 *
 * Returns the object locked when one is found. Parameters:
 *   ctp     : I/O service context of the requesting client
 *   prp_pid : pid of the requesting process
 *   prp     : requesting process (kernel-side handle)
 *   fd      : the fd to map
 *   msg     : the client's mmap request (may be NULL for a query)
 *   obpp    : OUT — receives the memory object (possibly NULL)
 * Return value: EOK on success, or an errno on failure.
 */
int vm_find_object(ioserv_context_t *ctp,
                   pid_t prp_pid,
                   process_t *prp,
                   int fd,
                   mem_map_t *msg,
                   path_object_t **obpp)
{
    struct _msg_info info;
    void *handle;
    const ioserv_ops_t *funcs;
    int i;
    int status;
    path_object_t *object;

    /* defends against invalid coid/fd range, vm_open_fd() will do validation
     * for those coid/fds which are within range.
     */
    if (fd < 0) {
        return EBADF;
    }

    info = ctp->info;
    info.coid = fd;
    object = NULL;

    funcs = ioserv_binding_funcs(ctp, &info);
    if (funcs) {
        handle = ioserv_ocb(ctp, &info);
        i = 0;
        for (;;) {
            status = fs_check[i](funcs, msg, handle, &object);
            if (status == -2) {
                /* File system recognized it, but wants root_vm_ops to treat it
                 * like a normal device
                 */
                funcs = NULL;
                object = NULL;
                break;
            }
            if (status == EOK)
                break;
            if (status != -1)
                return status;
            ++i;
            if (i >= NUM_ELTS(fs_check)) {
                /* No filesystem-specific handler recognized this fd. Fall
                 * through to vm_open_fd for generic file mapping.
                 */
                funcs = NULL;
                break;
            }
        }
    }

    if (object) {
        memobj_lock(object);
    }

    /* RUSH3: If msg is absent, we'd really like to query the memory mapped
     * RUSH3: file code to see if 'fd' is still open and pointing at RUSH3: a
     * memory mapped file, without creating one if it doesn't exist
     */
    if (!funcs && msg) {
        if (proc_thread_pool_reserve() != 0) {
            return EAGAIN;
        }
        status = vm_open_fd(ctp, prp_pid, prp, msg, &object);
        proc_thread_pool_reserve_done();
    } else {
        status = EOK;
    }
    *obpp = object;
    return status;
}

/* Service a _MEM_MAP request: validate, resolve the object, and map it.
 *
 * Normalizes the requested flags/protection, resolves anonymous, physical, or
 * fd-backed objects, then calls the active root VM mmap hook (retrying after a
 * compaction on transient ENOMEM/EAGAIN). Emits a trace event when
 * instrumentation is enabled and replies with the resulting address and real
 * size. Parameters:
 *   ctp     : I/O service context of the requesting client
 *   prp_pid : pid of the target process
 *   prp     : target process (kernel-side handle)
 *   msg     : the mmap request / reply buffer
 * Return value: a IOSERV_PTR reply on success, or an errno on failure.
 */
int vm_map(ioserv_context_t *ctp, pid_t prp_pid, process_t *prp, mem_map_t *msg)
{
    void *addr;
    size_t size;
    int status;
    path_object_t *object = NULL;
    int try_again;
    size_t pg_offset;
    pid_t pid;
    int handle_backed = 0;
    int created_anon_shared = 0;
    uint32_t handle_flags = 0;
    pid = prp_pid;

    switch (msg->i.flags & MAP_TYPE) {
    case 0:
        if (msg->i.flags & MAP_ANON) {
            /* Anonymous mappings without an explicit sharing type are private.
             */
            msg->i.flags |= MAP_PRIVATE;
        } else {
            return EINVAL;
        }
        break;
    case MM_MAP_PRIVATEANON:
        msg->i.flags =
            (msg->i.flags & ~(uint32_t) MAP_TYPE) | (MAP_ANON | MAP_PRIVATE);
        break;
    default:
        break;
    }
    if (msg->i.len == 0) {
        return EINVAL;
    }
    /* parm rather than prot like they should. */
    if (msg->i.prot & ~(uint32_t) PROT_MASK) {
        return EINVAL;
    }
    /* MAP_SYSRAM is just a status indicator - not allowed to pass it in. */
check_again:
    if (msg->i.flags & (PROT_MASK | MAP_SYSRAM | MAP_RESERVMASK | MM_PG_MASK)) {
        if (msg->i.flags & PROT_NOCACHE) {
            /* Kludge fix for test programs that pass PROT_NOCACHE in flags */
            msg->i.prot |= PROT_NOCACHE;
            msg->i.flags &= ~(uint32_t) PROT_NOCACHE;
            goto check_again;
        }
        return EINVAL;
    }
    if (msg->i.mem_handle != 0) {
        if (msg->i.fd != NOFD || (msg->i.flags & (MAP_ANON | MAP_PHYS)))
            return EINVAL;
        status = memcap_lookup_map(ctp, msg->i.mem_handle, msg->i.prot, &object,
                                   &handle_flags);
        if (status != EOK)
            return status;
        msg->i.flags |= handle_flags;
        handle_backed = 1;
    }
    if (!handle_backed && (msg->i.fd == NOFD) && !(msg->i.flags & MAP_PHYS)) {
        msg->i.flags |= MAP_ANON;
    }
    if (msg->i.flags & MAP_ANON) {
        if (msg->i.fd != NOFD) {
            return EINVAL;
        }
    } else if (msg->i.flags & MAP_PHYS) {
        paddr64_t end;

        if (!proc_isaccess(0, &ctp->info)) {
            /* This is the test for MAP_PRIVATE mappings. The test for
             * MAP_PHYS|MAP_SHARED should check permissions on /dev/mem, but
             * this more restrictive "must be root" check will do for now..
             */
            return EPERM;
        }
        if (msg->i.fd != NOFD) {
            return EINVAL;
        }
        end = ROUNDUP((uint64_t) msg->i.offset + msg->i.len, __PAGESIZE) - 1;
        if ((end < (uint64_t) msg->i.offset) || (end > last_paddr)) {
            /* attempt to map beyond end of physical memory. */
            return EINVAL;
        }
    } else if (!handle_backed) {
        status = vm_find_object(ctp, prp_pid, prp, msg->i.fd, msg, &object);
        if (status != EOK)
            return status;
    }

    /* DMA/MMIO containment: once an address space has self-restricted
     * (_PROC_PHYS_CONTAIN), every further specific-address MAP_PHYS is refused.
     * An LKL domain maps all of its device MMIO up front during hardware
     * discovery and needs no more afterward, so sealing here confines a
     * later-compromised domain to the register windows it already holds. This
     * runs after fd resolution too, so a fd-backed /dev/mem mapping — where
     * devmem_check adds MAP_PHYS inside vm_find_object above — cannot slip
     * past. MAP_ANON DMA buffers stay exempt (their MAP_ANON is still set here;
     * it is only stripped below). Inert for every process that never
     * self-restricts.
     */
    status = contained_phys_map_ok(prp_pid, msg);
    if (status != EOK) {
        if (object)
            memobj_unlock(object);
        return status;
    }

    if (!object &&
        ((msg->i.flags & (MAP_ANON | MAP_TYPE)) == (MAP_ANON | MAP_SHARED))) {
        /* anonymous shared object */
        object = object_create(OBJECT_MEM_SHARED, msg, pid);
        if (!object) {
            return ENOMEM;
        }
        created_anon_shared = 1;
        msg->i.flags &= ~(uint32_t) (MAP_ANON | MAP_PHYS);
        memobj_lock(object);
    }

    if (proc_thread_pool_reserve() != 0) {
        if (object) {
            memobj_unlock(object);
            if (created_anon_shared)
                (void) object_done(object);
        }
        return EAGAIN;
    }

    for (;;) {
        int lock_status;
        status = root_vm_ops.mmap(prp_pid, prp, msg->i.addr, msg->i.len,
                                  (int) msg->i.prot, (int) msg->i.flags, object,
                                  (uint64_t) msg->i.offset, msg->i.align,
                                  msg->i.preload, msg->i.fd, &addr, &size);
        if ((status != ENOMEM) && (status != EAGAIN))
            break;

        /* RUSH3: This only schedules the compaction, it doesn't actually RUSH3:
         * do it - we really should wait until complete, or at least RUSH3:
         * until something's been freed, and it needs to set RUSH3: try_again to
         * TRUE if it worked (assignment below should be |=).
         */
        (void) vm_fd_compact();
        process_bind(0);
        proc_unlock_adp(pid);
        /* @@@ We have to do this some other way. */
        try_again = 0; /* no swap device */
        prp = root_proc_lookup(pid);
        CRASHCHECK(!prp);

        /* We always aquire the as lock first, then the underlying object
         * (avoids deadlocks). Right now we've got the object locked and we need
         * to get the as back again. We need to release the object lock first.
         * The memobj_lock() that we originally did upped the reference count on
         * the object, so no one will delete it while we've got it unlocked
         */
        if (object) {
            proc_mux_unlock(&object->mem.mm.mux);
        }
        /* Now aquire the locks in the proper order. */
        lock_status = proc_wlock_adp(pid);
        CRASHCHECK(lock_status == -1);
        process_bind(pid);
        if (object) {
            proc_mux_lock(&object->mem.mm.mux);
        }
        if (!try_again)
            break;
    }

#if defined(CONFIG_INSTRUMENTATION)
    {
        iov_t iov[7];
        int iovcnt = 0;
        /* Overwrite the incoming address, we wipe it out shortly anyway */
        msg->i.addr = (uintptr_t) addr;
        SETIOV(iov + iovcnt, &pid, sizeof(pid));
        iovcnt++;
        SETIOV(iov + iovcnt, &msg->i.addr,
               sizeof(msg->i.addr) + sizeof(msg->i.len));
        iovcnt++;
        SETIOV(iov + iovcnt, &msg->i.flags, sizeof(msg->i.flags));
        iovcnt++;
        /* Wide mode only */
        SETIOV(iov + iovcnt, &msg->i.prot, sizeof(msg->i.prot));
        iovcnt++;
        SETIOV(iov + iovcnt, &msg->i.fd, sizeof(msg->i.fd));
        iovcnt++;
        SETIOV(iov + iovcnt, &msg->i.align,
               sizeof(msg->i.align) + sizeof(msg->i.offset));
        iovcnt++;
        if (object && object->hdr.type == OBJECT_MEM_FD && object->fdmem.name) {
            SETIOV(iov + iovcnt, object->fdmem.name,
                   strlen(object->fdmem.name));
            iovcnt++;
        }
        (void) kext_add_trace_event_iov(_TRACE_SYSTEM_C, RTMUX_TRACE_SYS_MMAP,
                                        iov, iovcnt);
    }
#endif

    proc_thread_pool_reserve_done();

    if (object) {
        memobj_unlock(object);
    }

    if (status) {
        if (created_anon_shared)
            (void) object_done(object);
        return status;
    }

    msg->o.addr = (uintptr_t) addr;
    pg_offset = (uintptr_t) addr % root_vm_ops.pagesize;
    msg->o.real_size = size + pg_offset;
    msg->o.real_addr = msg->o.addr - pg_offset;

    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}
