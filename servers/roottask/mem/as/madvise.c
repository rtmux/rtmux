/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"

int as_madvise(pid_t pid,
               process_t *prp,
               uintptr_t vaddr,
               size_t len,
               int flags)
{
    int r;
    struct map_set ms;
    struct mm_map *mm;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;

    if (!tmp)
        return EINVAL;

    r = map_isolate(&ms, &tmp->map, vaddr, len, MI_SPLIT);
    if (r != EOK)
        return r;

    for (mm = ms.first; mm != ms.last->next; mm = mm->next) {
        mm->extra_flags = (mm->extra_flags & ~(unsigned) EXTRA_FLAG_MADV_MASK) |
                          (unsigned) flags;
    }
    map_coalese(&ms);
    return EOK;
}
