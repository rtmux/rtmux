/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/memmsg.h>
#include "mm-internal.h"

static unsigned get_flags(ioserv_context_t *ctp, int fd)
{
    struct _msg_info info;
    void *handle;
    const ioserv_ops_t *funcs;
    path_object_t *object;
    mem_map_t msg;

    info = ctp->info;
    info.coid = fd;

    funcs = ioserv_binding_funcs(ctp, &info);
    if (!funcs)
        crash();
    handle = ioserv_ocb(ctp, &info);
    msg.i.flags = 0;
    if (devmem_check(funcs, &msg, handle, &object) == -1)
        crash();
    return msg.i.flags;
}


int vm_info(ioserv_context_t *ctp,
            pid_t prp_pid,
            process_t *prp,
            mem_info_t *msg)
{
    path_object_t *obp;
    int r;
    unsigned flags;
    paddr_t size;

    if (msg->i.fd != NOFD) {
        r = vm_find_object(ctp, prp_pid, prp, msg->i.fd, NULL, &obp);
        switch (r) {
        case EOK:
            if (obp)
                break;
            /* fall through */
        case ENOTSUP:
            r = ENODEV;
            /* fall through */
        default:
            return r;
        }
        switch (obp->hdr.type) {
        case OBJECT_MEM_TYPED:
            flags = get_flags(ctp, msg->i.fd);
            if (flags & IMAP_TYMEM_ALLOCATE) {
                tymem_free_info(obp, &size, NULL);
            } else if (flags & IMAP_TYMEM_ALLOCATE_CONTIG) {
                tymem_free_info(obp, NULL, &size);
            } else {
                size = 0;
            }
            memset(&msg->o, 0x00, sizeof msg->o);
            msg->o.info.posix_tmi_length = size;
            memobj_unlock(obp);
            return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
        default:
            break;
        }

        memobj_unlock(obp);
        return ENODEV;
    }
    if (!(msg->i.flags & MAP_ANON)) {
        switch (msg->i.flags & MAP_TYPE) {
        case MM_MAP_PRIVATEANON:
        case MAP_PRIVATE:
            break;
        case MAP_SHARED:
            return EINVAL;
        default:
            break;
        }
    }
    if (msg->i.flags & MAP_PHYS) {
        /* @@@ Don't support returning largest chuck yet */
        return EINVAL;
    }

    memset(&msg->o, 0x00, sizeof msg->o);
    msg->o.info.posix_tmi_length = pa_free_size();
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}
