/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"

int as_debuginfo(pid_t pid, process_t *prp, struct _mem_debug_info *info)
{
    struct map_set ms;
    struct mm_map *mm;
    struct mm_object_ref * or ;
    path_object_t *obp;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;
    int r;

    if (!tmp)
        return EINVAL;

    r = map_isolate(&ms, &tmp->map, info->vaddr, info->size, MI_NONE);
    if (r == EOK) {
        mm = ms.first;
        if (mm) {
            if (mm->mmap_flags & MAP_ELF) {
                or = mm->obj_ref;
                if (or) {
                    /* RUSH3: Do we actually need to store this? RUSH3: It just
                     * appears to be used by the old as_mapinfo().
                     */
                    mm->reloc = (intptr_t) (info->vaddr - info->old_vaddr);
                    obp = or->obp;
                    if (obp->hdr.type == OBJECT_MEM_FD) {
                        (void) vm_fd_setname(obp, info->path);
                    }
                }
            }
        }
        map_coalese(&ms);
    }
    return r;
}
