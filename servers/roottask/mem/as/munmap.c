/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"
#include "root-process.h"


struct unmap_data {
    pid_t pid;
    process_t *prp;
    struct mm_map *mm;
    unsigned flags;
    unsigned unmap_flags;
    unsigned check_limit;
    off64_t start;
    off64_t end;
};

static int clean_pmem(path_object_t *obp,
                      off64_t off,
                      struct pa_quantum *pq,
                      size_t num,
                      void *d)
{
    struct unmap_data *data = d;
    paddr_t paddr;
    off64_t skip;
    paddr_t sync_off;
    struct mm_map *mm;
    uintptr_t vaddr;

    mm = data->mm;
    paddr = pa_quantum_to_paddr(pq);
    if (pa_pq_blk(pq) == PAQ_BLK_FAKE) {
        /* If we've never granted write permission to any pages in the mapping,
         * we can't possibly have a sync object in the region
         */
        if (pa_pq_flags(pq) & PAQ_FLAG_MODIFIED) {
            /* We only have a single pa_quantum_fake to cover the whole of the
             * run, so we need to adjust the paddr with the offset from the
             * start.
             */
            if (off < mm->offset) {
                skip = mm->offset - off;
                paddr += (paddr_t) skip;
                num -= LEN_TO_NQUANTUM(skip);
                off = mm->offset;
            }
            sync_off = PADDR_TO_SYNC_OFF(paddr);
            /* Match as_vaddr_to_memobj: sync hash is keyed by prp */
            memobj_destroyed(
                (path_object_t *) data->prp, sync_off,
                sync_off + ((paddr_t) num << QUANTUM_BITS) - 1, data->prp,
                (void *) (mm->start + (uintptr_t) (off - mm->offset)));
        }
        data->flags |= PAQ_FLAG_INITIALIZED | PAQ_FLAG_INUSE;
    } else {
        vaddr = mm->start + (uintptr_t) (off - mm->offset);
        do {
            unsigned pq_flags;

            pq_flags = pa_pq_flags(pq);
            if (data->unmap_flags & MM_UNMAP_INIT_REQUIRED) {
                pa_pq_or_flags(pq, PAQ_FLAG_INIT_REQUIRED);
            }
            data->flags |= pq_flags | PAQ_FLAG_INUSE;
            if (pq_flags & PAQ_FLAG_HAS_SYNC) {
                process_t *prp = data->prp;

                sync_off = PADDR_TO_SYNC_OFF(paddr);
                /* Match as_vaddr_to_memobj: sync hash is keyed by prp */
                memobj_destroyed((path_object_t *) prp, sync_off,
                                 sync_off + (QUANTUM_SIZE - 1), prp,
                                 (void *) vaddr);
            }
            vaddr += QUANTUM_SIZE;
            paddr += QUANTUM_SIZE;
            ++pq;
        } while (--num != 0);
    }
    return EOK;
}

static int check_last_ref(struct mm_object_ref * or, struct mm_map *mm, void *d)
{
    struct unmap_data *data = d;
    off64_t mm_start;
    off64_t mm_end;

    if (data->mm == mm)
        return EOK;
    if (--data->check_limit == 0) {
        /* We're taking too long, just bail out. */
        data->start = data->end + 1;
        return -1;
    }
    mm_start = mm->offset;
    mm_end = mm->offset + (off64_t) (mm->end - mm->start);
    /* RUSH3: These tests for overlapping offsets can be more sophisticated */
    if (mm_start > data->end)
        return EOK;
    if (mm_end < data->start)
        return EOK;
    if (mm_start < data->end)
        data->end = mm_start;
    if (mm_end > data->start)
        data->start = mm_end;
    if (data->start > data->end)
        return -1;
    return EOK;
}


int ms_unmap(pid_t pid,
             address_t *adp,
             struct map_set *ms,
             unsigned unmap_flags)
{
    struct mm_map *mm;
    struct mm_map *end_mm;
    struct mm_object_ref * or ;
    struct unmap_data data;
    path_object_t *obp;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;
    root_proc_info_t prp_info;
    int prp_info_valid = 0;
    int haslock;
    bool has_unmapped = false;

    CRASHCHECK(!adp);
    data.pid = pid;
    data.prp = NULL;
    if (pid != 0 && process_query_info(pid, &prp_info) == EOK)
        prp_info_valid = 1;

    proc_lock_owner_mark(pid);

    if (!(unmap_flags & ~(UNMAP_PRIVATIZE | UNMAP_NORLIMIT))) {
        unmap_flags |= (unsigned) munmap_flags_default;
    }
    data.unmap_flags = unmap_flags;
    end_mm = ms->last->next;
    for (mm = ms->first; mm != end_mm; mm = mm->next) {
        if (mm->extra_flags & EXTRA_FLAG_SPECIAL) {
            if (!(prp_info_valid &&
                  (prp_info.flags &
                   (RTMUX_FLG_PROC_LOADING | RTMUX_FLG_PROC_TERMING)))) {
                ms->flags |= MI_SKIP_SPECIAL;
                continue;
            }
        }
        has_unmapped = true;
        if (!(data.unmap_flags & UNMAP_NORLIMIT)) {
            size_t size;

            size = (mm->end - mm->start) + 1;
            if (tmp) {
                tmp->rlimit.vmem -= size;
                if (mm->extra_flags & EXTRA_FLAG_RLIMIT_DATA) {
                    tmp->rlimit.data -= size;
                }
            }
        }
        or = mm->obj_ref;
        if (or) {
            obp = or->obp;
            haslock = memobj_cond_lock(obp);

            /* FUTURE: If we start turning on MM_MEM_HAS_SYNC FUTURE: in
             * as_vaddr_to_memobj(), we can check FUTURE: it here and avoid the
             * individual page checks
             */

            /* FUTURE: If the map region has never had write permissions,
             * FUTURE: we can skip this check, since we obviously can't FUTURE:
             * own a mutex: can't just check PROT_WRITE, since FUTURE: we might
             * have gotten the mutex and then turned off FUTURE: PROT_WRITE with
             * mprotect() - silly, but possible. FUTURE: The loader threads will
             * need to say that even though FUTURE: they had PROT_WRITE on for a
             * while, it doesn't count FUTURE: against this check. FUTURE: Since
             * clean_pmem is now doing more than just dealing FUTURE: with the
             * sync object, have to be more careful about FUTURE: skipping the
             * call.
             */

            data.mm = mm;
            data.flags = 0;
            (void) memobj_pmem_walk_mm(MPW_PHYS | MPW_SYSRAM, mm, clean_pmem,
                                       &data);

            if ((data.flags & PAQ_FLAG_MODIFIED) &&
                !(mm->mmap_flags & MAP_NOSYNCFILE) &&
                (obp->hdr.type == OBJECT_MEM_FD) &&
                (obp->mem.mm.flags & MM_FDMEM_NEEDSSYNC)) {
                /* RUSH1: What to do if mm_sync() returns non-EOK? */
                (void) mm_sync(obp, mm, MS_ASYNC);
            }

            /* RUSH3: The mm_sync() call is also going to walk the RUSH3: memory
             * reference list. Can we combine the two?
             */
            data.start = mm->offset;
            data.end = mm->offset + (off64_t) (mm->end - mm->start);
            data.check_limit = 100;
            /* RUSH3: Make more object oriented */
            switch (obp->hdr.type) {
            case OBJECT_MEM_ANON:
                if (obp->mem.mm.flags & MM_ANMEM_MULTI_REFS) {
                    /* We can't bail out early or we'll leak anon memory. */
                    data.check_limit = ~0U;
                } else {
                    /* We can skip check_last_ref() if it's an anonymous object
                     * with no mapping of "/proc/<pid>/as".
                     */
                    data.check_limit = 0;
                }
                break;
            case OBJECT_MEM_SHARED:
                if (obp->hdr.refs != 0) {
                    /* Can't free any memory yet. */
                    data.check_limit = 0;
                    data.start = data.end + 1;
                }
                break;
            case OBJECT_MEM_FD:
                /* RUSH3: If could tell if there were no open fd's, RUSH3: we
                 * could start freeing memory here.
                 */
                data.check_limit = 0;
                data.start = data.end + 1;
                break;
            case OBJECT_MEM_TYPED:
                /* RUSH3: Are there some conditions that will allow us RUSH3: to
                 * skip the check? MAP_PRIVATE? non-allocable? Can't bail out
                 * early or we'll leak memory
                 */
                data.check_limit = ~0U;
                break;
            default:
                break;
            }
            if (data.check_limit != 0) {
                memref_walk(obp, check_last_ref, &data);
            }

            {
                /* If none of the quantums has the PAQ_FLAG_INITIALIZED bit on
                 * (from clean_pmem), we know that we can't have set up any page
                 * table entries, and can skip the unmapping
                 */
                if (data.flags & PAQ_FLAG_INITIALIZED) {
                    pte_unmap(adp, mm->start, mm->end, obp);
                }
            }
            memref_del(mm);

            if (obp->hdr.type != OBJECT_MEM_ANON) {
                if (((mm->mmap_flags & MAP_TYPE) == MAP_PRIVATE) &&
                    !(data.unmap_flags & UNMAP_PRIVATIZE)) {
                    path_object_t *anmem_obp;
                    off64_t anmem_off;
                    size_t anmem_size;
                    int anmem_lock;

                    /* For MAP_PRIVATE, non-anonymous objects, we pre-allocated
                     * all potential anon memory for copy-on-write in as_mmap().
                     * Free that now.
                     */
                    anmem_obp = tmp ? tmp->anon : NULL;
                    if (!anmem_obp)
                        continue;
                    anmem_size = (mm->end - mm->start) + 1;
                    anmem_lock = memobj_cond_lock(anmem_obp);
                    anmem_off = anmem_offset(anmem_obp, mm->start, anmem_size);
                    memobj_pmem_del_len(anmem_obp, anmem_off, anmem_size);
                    if (!anmem_lock)
                        memobj_unlock(anmem_obp);
                }
            }

            if ((data.flags & PAQ_FLAG_INUSE) && (data.start < data.end)) {
                memobj_pmem_del_len(obp, data.start,
                                    (size_t) (data.end - data.start) + 1);
            }

            if (!haslock)
                memobj_unlock(obp);
        }
    }
    if (has_unmapped) {
        map_remove(ms);
        map_destroy(ms);
    } else {
        map_coalese(ms);
    }

    return EOK;
}


int as_munmap(pid_t pid, process_t *prp, uintptr_t vaddr, size_t len, int flags)
{
    int r;
    struct map_set ms;
    address_t *adp;
    root_process_t *tmp;

    if (pid == 0) {
        /* This code is assuming that all the memory we're unmapping was
         * pa_alloc'd - that is, a direct MAP_PHYS mapping never gets unmapped.
         * Right now the only time that kind of mapping is done is for mounting
         * an image file system and they never get unmounted, so we should be
         * safe for right now.
         */
        if (CPU_1TO1_IS_VADDR(vaddr)) {
            CPU_1TO1_FLUSH(vaddr, len);
            pa_free_paddr(vaddr - CPU_1TO1_VADDR_BIAS, len, 0);
        } else {
#if CPU_SYSTEM_PADDR_MUST
            crash();
#else
            va_rover = vaddr;
            do {
                paddr_t paddr;

                (void) as_vaddrinfo(0, NULL, vaddr, &paddr, NULL, VI_PGTBL);
                pte_unmap(NULL, vaddr, vaddr + (QUANTUM_SIZE - 1), NULL);
                pa_free_paddr(paddr, QUANTUM_SIZE, 0);
                vaddr += QUANTUM_SIZE;
                len -= QUANTUM_SIZE;
            } while (len != 0);
#endif
        }
        return EOK;
    }

    tmp = root_proc(pid);
    adp = tmp->adp;
    r = map_isolate(&ms, &tmp->map, vaddr, len, MI_SPLIT);
    if ((r == EOK) && (ms.first)) {
        r = ms_unmap(pid, adp, &ms, (unsigned) flags);
    }
    return r;
}
