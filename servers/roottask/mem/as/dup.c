/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic.h>
#include "as.h"
#include "pa-pq.h"
#include "root-process.h"

/* RUSH3: Duping a [data=uip] executable from an IFS don't work too well. RUSH3:
 * Can we detect that and error out?
 */

/* This call dups the address space from process 'pprp' into process 'prp' */

struct dup_data {
    off64_t off;
    struct mm_map *mm;
    struct mm_map *pmm;
    address_t *adp;
    pid_t target_pid;
    uintptr_t va;
    size_t va_off;
};


/* Copy one contiguous run of source data into the child's as.
 *
 * Callback for pte_temp_map; advances data->va_off by len. Parameters:
 *   src : kernel-direct-map source address
 *   len : number of bytes to copy
 *   d   : dup_data context
 * Return value: EOK on success, else an error code
 */
static int data_dup_run(void *src, size_t len, void *d)
{
    struct dup_data *data = d;
    pid_t target_pid;
    int r;

    /* dst (data->va + data->va_off) is a user VA in the CHILD process's as. In
     * U-mode roottask the satp stays loaded with roottask's own pgdir, so a
     * direct memcpy to that VA would fault. as_memcpy switches satp on the
     * kernel side, copies under xfer fault recovery, restores satp. src here is
     * a kernel-direct-map VA from pte_temp_map(), which remains accessible
     * across the satp swap.
     */
    target_pid = data->target_pid;
    r = as_memcpy(target_pid, (void *) (data->va + data->va_off), src, len,
                  AS_MEMCPY_TO_TARGET);
    if (r != EOK)
        return r;
    data->va_off += len;
    return EOK;
}


/* Copy modified pages of a memory object into the child's as.
 *
 * Callback for memobj_pmem_walk: skips unmodified quanta, then copies each
 * contiguous run of modified memory (temporarily adding PROT_WRITE so the
 * destination is writable). Parameters:
 *   obp : memory object being walked
 *   off : object offset of the first quantum
 *   pq  : array of page-allocation quanta
 *   num : number of quanta
 *   d   : dup_data context
 * Return value: EOK on success, else an error code
 */
static int data_dup(path_object_t *obp,
                    off64_t off,
                    struct pa_quantum *pq,
                    size_t num,
                    void *d)
{
    struct dup_data *data = d;
    struct mm_map *mm;
    struct mm_object_ref * or ;
    int r;
    unsigned orig_flags;
    uintptr_t start;
    uintptr_t end;
    unsigned run;

    mm = data->mm;
    orig_flags = mm->mmap_flags;
    for (;;) {
        /* Skip over quantum's that haven't been modified yet (we know they're
         * all zeros).
         */
        for (;;) {
            if (num == 0)
                return EOK;
            if (pa_pq_flags(pq) & PAQ_FLAG_MODIFIED)
                break;
            ++pq;
            off += QUANTUM_SIZE;
            --num;
        }
        /* Find the length of the next contiguous run of modified memory */
        run = 0;
        for (;;) {
            ++run;
            if (!(pq[run].flags & PAQ_FLAG_MODIFIED))
                break;
            if (run == num)
                break;
        }
        /* Copy the modified memory to the child */
        data->va_off = (size_t) (off - data->pmm->offset);
        start = data->va + data->va_off;
        end = start + NQUANTUM_TO_LEN(run) - 1;

        /* Add PROT_WRITE to make sure we can write the storage. */
        mm->mmap_flags = orig_flags | PROT_WRITE;

        r = memory_reference(&mm, start, end, MR_WRITE, NULL);
        if (r != EOK)
            return r;
        r = pte_temp_map(data->adp, start, pq, data->pmm, (end - start) + 1,
                         data_dup_run, data);
        or = mm->obj_ref;
        if (!(orig_flags & PROT_WRITE)) {
            (void) pte_prot(or->adp, start, end, orig_flags, or->obp);
        }
        (void) orig_flags;
        mm->mmap_flags = orig_flags;
        if (r != EOK)
            return r;
        off += run << QUANTUM_BITS;
        pq += run;
        num -= run;
    }
}


/* Duplicate the address space of a parent process into a child.
 *
 * Tears down the child's as_mcreate() mappings (keeping any EXTRA_FLAG_SPECIAL
 * entries), then walks the parent's map list and recreates each inheritable
 * region in the child — copying modified pages and setting up copy-on-write
 * anonymous backing where needed. Parameters:
 *   pprp_pid : parent process id
 *   pprp     : parent process
 *   prp_pid  : child process id
 *   prp      : child process
 * Return value: EOK on success, else an error code
 */
int as_dup(pid_t pprp_pid, process_t *pprp, pid_t prp_pid, process_t *prp)
{
    address_t *padp;
    struct map_set pms;
    struct mm_map *pmm;
    struct mm_object_ref *por;
    path_object_t *pobp;
    address_t *adp;
    struct map_set ms;
    struct mm_map *mm;
    path_object_t *anon;
    struct map_set repl_ms;
    int r;
    uintptr_t vaddr;
    size_t len;
    struct dup_data data;
    root_process_t *parent_tmp = (pprp_pid != 0) ? root_proc(pprp_pid) : NULL;
    root_process_t *child_tmp = (prp_pid != 0) ? root_proc(prp_pid) : NULL;

    if (!parent_tmp || !child_tmp)
        return EINVAL;

    padp = parent_tmp->adp;
    if (!padp)
        return EOK;

    adp = child_tmp->adp;
    if (!adp)
        return ENOMEM;

    /* Clean up all the entries created by the as_mcreate() code except for
     * EXTRA_FLAG_SPECIAL one(s) (e.g. MIPS syspage). We'll recreate the ones we
     * want from the parent. We have to leave the EXTRA_FLAG_SPECIAL one(s) in
     * place so that the temporary mapping code doesn't use those vaddr regions.
     * They'll get cleaned up in the copy loop when we get to that point in the
     * address space.
     */
    vaddr = 0;
    for (;;) {
        r = map_isolate(&ms, &child_tmp->map, vaddr, 0, MI_NEXT);
        CRASHCHECK(r != EOK);
        mm = ms.first;
        if (!mm)
            break;
        vaddr = mm->end + 1;
        if (!(mm->extra_flags & EXTRA_FLAG_SPECIAL)) {
            r = ms_unmap(prp_pid, adp, &ms, 0);
            CRASHCHECK(r != EOK);
        }
    }

    proc_lock_owner_mark(prp_pid);

    memset(&child_tmp->rlimit, 0, sizeof(child_tmp->rlimit));
    data.adp = adp;
    data.target_pid = prp_pid;
    vaddr = 0;
    for (;;) {
        r = map_isolate(&pms, &parent_tmp->map, vaddr, 0, MI_NEXT);
        if (r != EOK)
            goto fail1;
        pmm = pms.first;
        if (!pmm)
            break;

        /* Yeah, it's a double negative for the test, but doing it this way
         * saves code in mm-map.c since the default has the flag off.
         */
        if (!(pmm->extra_flags & EXTRA_FLAG_NOINHERIT)) {
            len = (pmm->end - pmm->start) + 1;
            r = map_isolate(&repl_ms, &child_tmp->map, pmm->start, len,
                            MI_SPLIT);
            if (r != EOK)
                goto fail1;
            if (repl_ms.first) {
                CRASHCHECK(!(repl_ms.first->extra_flags & EXTRA_FLAG_SPECIAL));

                /* Clean up an EXTRA_FLAG_SPECIAL entry in the child that was
                 * created by as_mcreate(). We're going to transfer the one from
                 * the parent now.
                 */

                r = ms_unmap(prp_pid, adp, &repl_ms, 0);
                CRASHCHECK(r != EOK);
            }
            r = map_create(&ms, NULL, &child_tmp->map, pmm->start, len, 0,
                           MAP_FIXED);
            if (r != EOK)
                goto fail1;
            mm = ms.first;
            mm->mmap_flags = pmm->mmap_flags;
            mm->extra_flags = pmm->extra_flags;
            mm->offset = pmm->offset;
            mm->reloc = pmm->reloc;
            mm->last_page_bss = pmm->last_page_bss;
            /* MAPFIELDS: copy mm_map fields from 'pmm' to 'mm' */

            child_tmp->rlimit.vmem += len;

            pobp = NULL;
            anon = NULL;
            r = map_add(&ms);
            if (r != EOK)
                goto fail3;
            por = pmm->obj_ref;
            if (por) {
                if (pmm->mmap_flags & EXTRA_FLAG_RLIMIT_DATA) {
                    child_tmp->rlimit.data += len;
                } else if (pmm->extra_flags & EXTRA_FLAG_PRIMARY_STK) {
                    child_tmp->rlimit.stack += len;
                }
                pobp = por->obp;
                memobj_lock(pobp);
                if ((pobp->hdr.type == OBJECT_MEM_ANON) &&
                    ((pmm->mmap_flags & MAP_TYPE) == MAP_PRIVATE)) {
                    anon = child_tmp->anon;
                    memobj_lock(anon);
                    r = memref_add(mm, anon, prp_pid, adp, NOFD);
                } else {
                    /* For MAP_PRIVATE, non anonymous memory, pre-allocate all
                     * anonymous memory we might need for copy-on-write.
                     */
                    if ((pmm->mmap_flags & MAP_TYPE) == MAP_PRIVATE) {
                        off64_t anmem_off;

                        memobj_lock(child_tmp->anon);
                        anmem_off =
                            anmem_offset(child_tmp->anon, pmm->start, len);
                        r = memobj_pmem_add(child_tmp->anon, anmem_off, len,
                                            (int) pmm->mmap_flags);
                        memobj_unlock(child_tmp->anon);
                        if (r != EOK)
                            goto fail4;
                    }
                    r = memref_add(mm, pobp, prp_pid, adp, por->fd);
                }
                if (r != EOK)
                    goto fail4;
            }
            if (anon) {
                mm->offset = anmem_offset(anon, mm->start, len);
                data.mm = mm;
                data.pmm = pmm;
                data.va = mm->start;
                data.off = mm->offset;
                r = memobj_pmem_add(anon, mm->offset, len,
                                    (int) mm->mmap_flags);
                if (r != EOK)
                    goto fail2;

                /* RUSH3: Eventually we should be able to (optionally) delay
                 * RUSH3: the copying of the data until first write.... RUSH3:
                 * Remember to deal with the parent having an ISR.
                 */
                r = memobj_pmem_walk(
                    MPW_SYSRAM, pobp, pmm->offset,
                    (off64_t) ((uint64_t) pmm->offset + len - 1), data_dup,
                    &data);
                if (r != EOK)
                    goto fail2;
                memobj_unlock(anon);
            }
            if (pobp) {
                /* Make sure that we have room for the L2 page table(s) */
                r = pte_prealloc(adp, mm->start, mm->end);
                if (r != EOK)
                    goto fail2;

                if ((mm->mmap_flags & MAP_TYPE) == MAP_SHARED) {
                    r = memory_reference(&mm, mm->start, mm->end, MR_NOINIT,
                                         NULL);
                    if (r != EOK)
                        goto fail2;
                }
                memobj_unlock(pobp);
            }
            map_coalese(&ms);
            map_coalese(&pms);
        }
        vaddr = pmm->end + 1;
    }

    return EOK;

fail4:
    map_remove(&ms);

fail3:
    map_destroy(&ms);

fail2:
    if (pobp)
        memobj_unlock(pobp);
    if (anon)
        memobj_unlock(anon);

fail1:
    return r;
}
