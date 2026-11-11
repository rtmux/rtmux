/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"

int as_validate(pid_t pid,
                process_t *prp,
                uintptr_t vaddr,
                size_t len,
                int flags)
{
    int r;
    struct map_set ms;
    struct mm_map *mm;
    struct mm_map *next;
    uintptr_t end;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;
    root_proc_info_t prp_info;
    int prp_info_valid;

    if (!tmp)
        return EINVAL;
    prp_info_valid = (process_query_info(pid, &prp_info) == EOK);
    end = vaddr + len - 1;

    if (flags & VV_RANGE) {
        if ((end < vaddr) || (vaddr < tmp->map.start) || (end > tmp->map.end)) {
            /* If VV_MAPPED is off, we're doing munmap(), which wants EINVAL */
            return (flags & VV_MAPPED) ? ENOMEM : EINVAL;
        }
    }

    if (!(flags & VV_MAPPED))
        return EOK;

    r = map_isolate(&ms, &tmp->map, vaddr, len, MI_NONE);
    if (r != EOK)
        goto fail1;

    r = ENOMEM; /* Assume something's going to mess up */
    mm = ms.first;
    if (!mm)
        goto fail2;
    if ((vaddr < mm->start) || (end > ms.last->end))
        goto fail2;

    for (;;) {
        /* Attempting to fiddle with the system page address. No-no unless we're
         * terminating.
         */
        if ((mm->extra_flags & EXTRA_FLAG_SPECIAL) &&
            !(prp_info_valid && (prp_info.flags & RTMUX_FLG_PROC_TERMING))) {
            goto fail2;
        }
        if (mm == ms.last)
            break;
        next = mm->next;
        if ((mm->end + 1) != next->start)
            goto fail2;
        mm = next;
    }

    r = EOK; /* Everything's good */
fail2:
    map_coalese(&ms);
fail1:
    return r;
}
