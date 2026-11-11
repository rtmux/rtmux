/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"



int anmem_create(path_object_t *obp, void *extra)
{
    (void) extra;

    obp->mem.mm.restriction = restrict_user;
    obp->mem.mm.pmem_cache = NULL; /* start at head */
    return EOK;
}

int anmem_done(path_object_t *obp)
{
    if (obp->mem.mm.refs)
        return 0;
    if (obp->mem.mm.pmem) {
#ifndef NDEBUG

        /* We can't lock since the as might be being destroyed and the
         * __tls()->owner for mutex acquisition will not get the right value, so
         * we turn on the MM_MEM_SKIPLOCKCHECK to prevent crash()'s. It's safe
         * since nobody else is referencing this object.
         */
        obp->mem.mm.flags |= MM_MEM_SKIPLOCKCHECK;
#endif
        memobj_pmem_del_len(obp, 0, (uint64_t) obp->mem.mm.size);
    }
    return 1;
}


size_t anmem_name(path_object_t *obp, size_t max, char *name)
{
    return 0;
}


off64_t anmem_offset(path_object_t *obp, uintptr_t va, size_t size)
{
    uintptr_t end = va + size;

    VERIFY_OBJ_LOCK(obp);

    /* RUSH2: Using the vaddr as the offset won't work once we RUSH2: start
     * supporting /proc/<pid>/as, since the pmem for the RUSH2: va might still
     * be mapped via another process. RUSH2: When we switch, have to deal with
     * demand paging somehow. RUSH2: Maybe keep the offset, but create multiple
     * anon objects instead?
     */

    /* ms_unmap() (munmap.c) assumes there is only one anonymous object and that
     * anmem_offset() returns the same offset every time it receives the same
     * 'va'. The assumption is used when freeing preallocated demand-paging /
     * MAP_PRIVATE privatize() memory. If that changes, update ms_unmap() and
     * privatize() in mm-reference.c together.
     */
    if (end > (uintptr_t) obp->mem.mm.size)
        obp->mem.mm.size = (off64_t) end;
    return (off64_t) va;
}


void anmem_unoffset(path_object_t *obp, off64_t off, size_t size)
{
    VERIFY_OBJ_LOCK(obp);

    /* RUSH2: nothing to do right now. See above. RUSH2: Might turn out better
     * passing the va in, rather than the offset.
     */
}
