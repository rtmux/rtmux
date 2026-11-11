/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define _FILE_OFFSET_BITS 64
#include <sys/memmsg.h>
#include <sys/procfs.h>
#include "mm-internal.h"


int vm_offset(ioserv_context_t *ctp,
              pid_t prp_pid,
              process_t *prp,
              mem_offset_t *msg)
{
    paddr_t paddr;
    size_t len;
    procfs_mapinfo info;
    path_object_t *obp;
    path_object_t *fd_obp;
    size_t len_requested;
    size_t got;
    int fd;
    unsigned flags;

    len_requested = msg->i.len;
    switch (msg->i.subtype) {
    case _MEM_OFFSET_PT:
        flags = VI_PGTBL;
        goto get_info;
    case _MEM_OFFSET_PHYS:
        flags = VI_INIT;
    get_info:
        if (root_vm_ops.vaddrinfo(prp_pid, prp, msg->i.addr, &paddr, &len,
                                  flags) == PROT_NONE) {
            return EACCES;
        }
        msg->o.offset = (off64_t) paddr;
        msg->o.fd = -1;
        break;

    case _MEM_OFFSET_FD:
        got = root_vm_ops.mapinfo(prp_pid, prp, msg->i.addr, &info, NULL, 0,
                                  &obp, &fd, &len);
        if ((got == 0) || (msg->i.addr < info.vaddr) || !obp) {
            return EACCES;
        }
        switch (obp->hdr.type) {
        case OBJECT_MEM_ANON:
            return EACCES;
        case OBJECT_MEM_TYPED:
        case OBJECT_MEM_SHARED:
        case OBJECT_MEM_FD:
            if (fd != NOFD) {
                /* Check if 'fd' is still open and pointing at the object. */
                if ((vm_find_object(ctp, prp_pid, prp, fd, NULL, &fd_obp) ==
                     EOK) &&
                    fd_obp) {
                    memobj_unlock(fd_obp);
                    if (fd_obp != obp)
                        fd = NOFD;
                } else {
                    fd = NOFD;
                }
            }
            msg->o.fd = fd;
            break;
        default:
            crash();
            break;
        }
        msg->o.offset =
            (off64_t) ((size_t) info.offset + msg->i.addr - info.vaddr);
        break;
    default:
        return ENOSYS;
    }
    msg->o.size = min(len_requested, len);
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}
