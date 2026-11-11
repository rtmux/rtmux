/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <devctl/proc.h>
#include "mm-internal.h"


int shmem_create(path_object_t *obp, void *d)
{
    mem_map_t *msg = d;
    int r;

    /* RUSH3: Interface break */
    obp->mem.pm.mtime = time(0);
    /* RUSH2: Need a way to inherit/modify the restrict list. */
    obp->mem.mm.restriction = restrict_user;
    obp->mem.mm.pmem_cache = NULL; /* start at head */
    obp->shmem.vaddr = VA_INVALID;
    if (msg) {
        if (msg->i.flags & MAP_ANON) {
            if (msg->i.flags & MAP_PHYS) {
                obp->mem.mm.flags =
                    SHMCTL_ANON | SHMCTL_PHYS | MM_SHMEM_SPECIAL;
            }
            if (msg->i.flags & MM_MAP_NOX64K) {
                obp->mem.mm.flags |= SHMCTL_NOX64K | MM_SHMEM_SPECIAL;
            }
            path_object_clone(obp);
            proc_mux_lock(&obp->mem.mm.mux);
            r = root_vm_ops.resize(obp, msg->i.len);
            proc_mux_unlock(&obp->mem.mm.mux);
            path_object_unclone(obp);
            if (r != EOK)
                return r;
        } else if (msg->i.flags & MAP_PHYS) {
            obp->mem.mm.size = (off64_t) msg->i.len;
            obp->mem.mm.flags = MM_SHMEM_SPECIAL | SHMCTL_PHYS | MM_SHMEM_IFS;
            path_object_clone(obp);
            proc_mux_lock(&obp->mem.mm.mux);
            r = root_vm_ops.memobj_phys(obp, (paddr_t) msg->i.offset);
            proc_mux_unlock(&obp->mem.mm.mux);
            path_object_unclone(obp);
            if (r != EOK)
                return r;
        } else {
            crash();
        }
    }
    return EOK;
}


int shmem_done(path_object_t *obp)
{
    if (obp->mem.mm.refs)
        return 0;
    (void) root_vm_ops.resize(obp, 0);
    return 1;
}


size_t shmem_name(path_object_t *obp, size_t max, char *dest)
{
    return path_object_pathname(obp, max, dest);
}


int shmem_devctl(ioserv_context_t *ctp, io_devctl_t *msg, path_object_t *obp)
{
    union {
        unsigned ptr;
        devctl_mem_object memobj;
    } *data = (void *) (msg + 1);
    struct _client_info info;
    paddr64_t end;
    int r;

    switch ((unsigned) msg->i.devctl_cmd) {
    case DEVCTL_MEM_OBJECT_SET:
        r = 0;
        if ((obp->mem.mm.flags & MM_SHMEM_SPECIAL) || obp->mem.mm.pmem) {
            /* The object is already "special" */
            r = -1;
        } else if (connect_client_info(ctp->info.scoid, &info, 0) == -1) {
            r = -1;
        } else if (!proc_isaccess(0, &ctp->info) &&
                   ((data->memobj.flags & (SHMCTL_ANON | SHMCTL_PHYS)) ==
                    SHMCTL_PHYS)) {
            /* Should check permissions on /dev/mem, but this more restrictive
             * "must be root" check will do for now..
             */
            r = -1;
        } else {
            memobj_lock(obp);
            if ((obp->mem.mm.flags &
                 (SHMCTL_PHYS | SHMCTL_GLOBAL | SHMCTL_HAS_SPECIAL)) &&
                (info.cred.euid != 0)) {
                r = -1;
            } else {
                obp->mem.mm.flags |=
                    (data->memobj.flags & SHMCTL_FLAG_MASK) | MM_SHMEM_SPECIAL;
                obp->shmem.special = data->memobj.special;
                if (obp->mem.mm.flags & SHMCTL_ANON) {
                    if ((obp->mem.mm.flags & (SHMCTL_PHYS | SHMCTL_LAZY)) ==
                        (SHMCTL_PHYS | SHMCTL_LAZY)) {
                        /* Asking for lazy allocation of physically contiguous
                         * memory doesn't make sense
                         */
                        r = -1;
                    } else if (data->memobj.size > ~(size_t) 0) {
                        r = -1;
                    } else if (root_vm_ops.resize(obp, data->memobj.size) !=
                               EOK) {
                        r = -1;
                    }
                } else if ((obp->mem.mm.flags & SHMCTL_PHYS) &&
                           !(data->memobj.size & (root_vm_ops.pagesize - 1)) &&
                           !(data->memobj.offset &
                             (root_vm_ops.pagesize - 1))) {
                    /* Set our memobj to be the offset and size specified */
                    obp->mem.mm.size = (off64_t) data->memobj.size;
                    end = ROUNDUP((uint64_t) data->memobj.offset +
                                      data->memobj.size,
                                  __PAGESIZE) -
                          1;
                    if ((end < (uint64_t) data->memobj.offset) ||
                        (end > last_paddr)) {
                        r = -1;
                    } else if (root_vm_ops.memobj_phys(
                                   obp, (paddr_t) data->memobj.offset) != EOK) {
                        r = -1;
                    }
                } else {
                    r = -1;
                }
            }
            memobj_unlock(obp);
        }
        msg->o.ret_val = r;
        break;
    default:
        return IOSERV_DEFAULT;
    }
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}
