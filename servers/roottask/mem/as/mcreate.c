/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "root-process.h"
#include <spawn.h>

/* as_mcreate_pid — initialize the roottask half of a process as.
 *
 * The kernel's mm_mcreate (kernel/mm/mcreate.c) has already allocated the
 * address_t and per-CPU pgdir at __KSYS_PRIV_PROCESS_CREATE time; that
 * kernel-side struct holds only "cpu" state. Everything else (mm_map list,
 * OBJECT_MEM_ANON backing store, rlimit accounting, optional zero-vaddr
 * lock-out) lives in root_process_t and is initialized here. Roottask calls
 * this from the process_create wrapper right after the kernel extension
 * returns.
 *
 * Takes a pid (not a prp) because the kernel-allocated process_t is unreachable
 * from U-mode roottask. Parameters:
 *   pid : process whose roottask-side as state to initialize
 * Return value: EOK on success (including if already initialized), or
 *               an errno on failure.
 */
int as_mcreate_pid(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    path_object_t *anon;
    int r;
    uintptr_t end;
#if CPU_USER_VADDR_START == 0
    struct map_set ms;
    struct map_set repl;
#endif

    /* Already initialized? (Re-spawn / fork path may revisit.) */
    if (tmp->anon)
        return EOK;

    anon = object_create(OBJECT_MEM_ANON, tmp, 0);
    if (!anon)
        return ENOMEM;
    tmp->anon = path_object_clone(anon);

    /* Nip off a couple of the vaddrs at the end to use as a last chance area
     * for temp mappings. See mm-temp-map.c for details.
     */
    end = CPU_USER_VADDR_END - __PAGESIZE;
    r = map_init(&tmp->map, CPU_USER_VADDR_START, end);
    if (r != EOK)
        goto fail2;
#if CPU_USER_VADDR_START == 0

    /* pre-allocate the zero vaddr so user doesn't get it from a mmap() unless
     * he explicitly asks for it.
     */

    r = map_create(&ms, &repl, &tmp->map, 0, __PAGESIZE, 0, MAP_FIXED);
    if (r != EOK)
        goto fail3;
    r = map_add(&ms);
    if (r != EOK)
        goto fail4;
    tmp->rlimit.vmem = __PAGESIZE;
#endif

    return EOK;

#if CPU_USER_VADDR_START == 0
fail4:
    map_destroy(&ms);

fail3:
#endif
    (void) map_fini(&tmp->map);

fail2:
    path_object_done(anon);
    tmp->anon = NULL;
    return r;
}


/* as_mdestroy_pid — undo as_mcreate_pid once the process is unmapped.
 *
 * Drops the anonymous-memory object, which frees any pages it still holds that
 * no mapping covered at exit, and returns the map entries to the map pool.
 * Process exit used to leave both behind, keeping a few pages of physical
 * memory and a map block per exited process. Call with the process's
 * address-space write lock held, after the whole range was unmapped; an entry
 * that still references an object keeps the map (the unmap did not finish).
 * Parameters:
 *   pid : process whose roottask-side address-space state to release
 */
void as_mdestroy_pid(pid_t pid)
{
    root_process_t *tmp = root_proc(pid);
    path_object_t *anon = tmp->anon;
    struct mm_map *mm;

    for (mm = tmp->map.head; mm; mm = mm->next)
        if (mm->obj_ref)
            break;
    if (mm) {
        /* The unmap did not finish: keep the map and its object rather than
         * free memory something still references, and say so.
         */
        root_warn("as_mdestroy_pid: pid %d still maps an object after exit unmap\n",
                  (int) pid);
        return;
    }
    (void) map_fini(&tmp->map);
    if (anon) {
        tmp->anon = NULL;
        path_object_done(anon);
    }
}
