/*
 * RTMux roottask memory-object capability handles.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include "mm-internal.h"
#include "root-process.h"

#define MEMCAP_SLOTS 64U

static int memcap_ensure(root_process_t *tmp)
{
    rtmux_cap_slot_t *slots;

    if (tmp->mem_cap_slots)
        return EOK;

    slots = calloc(MEMCAP_SLOTS, sizeof(*slots));
    if (!slots)
        return ENOMEM;
    if (rtmux_cap_table_init(&tmp->mem_caps, slots, MEMCAP_SLOTS) != EOK) {
        free(slots);
        return EINVAL;
    }
    tmp->mem_cap_slots = slots;
    return EOK;
}

static unsigned memcap_rights_from_prot(unsigned prot)
{
    unsigned rights = 0;

    if (prot & PROT_READ)
        rights |= RTMUX_CAP_MEM_MAP_READ;
    if (prot & PROT_WRITE)
        rights |= RTMUX_CAP_MEM_MAP_WRITE;
    if (prot & PROT_EXEC)
        rights |= RTMUX_CAP_MEM_MAP_EXEC;
    return rights ? rights : RTMUX_CAP_MEM_MAP_READ;
}

static unsigned memcap_prot_from_rights(unsigned rights)
{
    unsigned prot = 0;

    if (rights & RTMUX_CAP_MEM_MAP_READ)
        prot |= PROT_READ;
    if (rights & RTMUX_CAP_MEM_MAP_WRITE)
        prot |= PROT_WRITE;
    if (rights & RTMUX_CAP_MEM_MAP_EXEC)
        prot |= PROT_EXEC;
    return prot;
}

static int memcap_find_fd_object(ioserv_context_t *ctp,
                                 int fd,
                                 unsigned rights,
                                 path_object_t **obpp,
                                 uint32_t *flagsp)
{
    struct _msg_info info;
    const ioserv_ops_t *funcs;
    path_object_t *object = NULL;
    mem_map_t probe;
    void *handle;
    int status;

    if (fd < 0)
        return EBADF;

    info = ctp->info;
    info.coid = fd;
    funcs = ioserv_binding_funcs(ctp, &info);
    if (!funcs)
        return EBADF;
    handle = ioserv_ocb(ctp, &info);

    memset(&probe, 0, sizeof(probe));
    probe.i.type = _MEM_MAP;
    probe.i.fd = fd;
    probe.i.flags = MAP_SHARED;
    probe.i.prot = memcap_prot_from_rights(rights);

    status = devmem_check(funcs, &probe, handle, &object);
    if (status == EACCES && object && object->hdr.type == OBJECT_MEM_TYPED &&
        (rights & RTMUX_CAP_MEM_MAP_WRITE) == 0) {
        /* Typed-memory opens in this tree can carry POSIX O_RDONLY as a zero
         * access mode in the OCB even though the fd is a valid read mapping
         * authority. Preserve the write-denial path, but allow read-only caps.
         */
        status = EOK;
    }
    if (status == -1)
        status = devzero_check(funcs, &probe, handle, &object);
    if (status == -1)
        return ENODEV;
    if (status != EOK)
        return status;
    if (!object)
        return ENODEV;

    *obpp = object;
    if (flagsp)
        *flagsp = probe.i.flags &
                  (IMAP_OCB_RDONLY | IMAP_TYMEM_ALLOCATE |
                   IMAP_TYMEM_ALLOCATE_CONTIG | IMAP_TYMEM_MAP_ALLOCATABLE);
    return EOK;
}

int vm_handle(ioserv_context_t *ctp,
              pid_t prp_pid,
              process_t *prp,
              mem_handle_t *msg)
{
    root_process_t *tmp = root_proc(prp_pid);
    path_object_t *object = NULL;
    void *dead = NULL;
    uint32_t flags;
    rtmux_cap_t cap;
    int status;

    (void) prp;

    switch (msg->i.subtype) {
    case _MEM_HANDLE_CREATE:
        if ((msg->i.rights & ~RTMUX_CAP_MEM_MAP_ALL) != 0 || msg->i.rights == 0)
            return EINVAL;
        flags = 0;
        status = memcap_find_fd_object(ctp, msg->i.fd, msg->i.rights, &object,
                                       &flags);
        if (status != EOK)
            return status;
        status = memcap_ensure(tmp);
        if (status != EOK)
            return status;
        path_object_clone(object);
        status = rtmux_cap_alloc_context(&tmp->mem_caps, object, flags,
                                         msg->i.rights, &cap);
        if (status != EOK) {
            path_object_done(object);
            return status;
        }
        msg->o.handle = cap;
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));

    case _MEM_HANDLE_CLOSE:
        if (!tmp->mem_cap_slots)
            return EBADF;
        status = rtmux_cap_revoke(&tmp->mem_caps, msg->i.handle, &dead);
        if (dead)
            path_object_done(dead);
        return status == EOK ? EOK : EBADF;

    default:
        return EINVAL;
    }
}

int memcap_lookup_map(ioserv_context_t *ctp,
                      uint64_t handle,
                      unsigned prot,
                      path_object_t **obpp,
                      uint32_t *flagsp)
{
    root_process_t *tmp = root_proc(ctp->info.pid);
    uint64_t context = 0;
    void *dead;
    void *object = NULL;
    unsigned rights;
    int status;

    if (!tmp->mem_cap_slots)
        return EBADF;

    rights = memcap_rights_from_prot(prot);
    status = rtmux_cap_lookup_pin_context(&tmp->mem_caps, handle, rights,
                                          &object, &context);
    if (status != EOK)
        return EBADF;

    memobj_lock(object);
    dead = rtmux_cap_unpin(&tmp->mem_caps, handle);
    if (dead)
        path_object_done(dead);
    *obpp = object;
    if (flagsp)
        *flagsp = (uint32_t) context;
    return EOK;
}

void memcap_process_fini(root_process_t *tmp)
{
    rtmux_cap_slot_t *slots = tmp->mem_cap_slots;

    if (!slots)
        return;

    /* Reads slot fields without the cap table lock, which is only sound here:
     * the owning process is already dead, so no lookup, revoke, or transfer can
     * still reach this table. Anywhere else, go through the rtmux_cap_* API.
     */
    for (uint32_t i = 1; i < tmp->mem_caps.nslots; i++) {
        if ((slots[i].state == RTMUX_CAP_LIVE ||
             slots[i].state == RTMUX_CAP_REVOKING) &&
            slots[i].object)
            path_object_done(slots[i].object);
    }
    free(slots);
    tmp->mem_cap_slots = NULL;
}
