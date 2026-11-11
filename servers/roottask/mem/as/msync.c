/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"
#include "root-process.h"

struct sync_data {
    off64_t off;
    struct mm_map *mm;
    int got;
    unsigned bits;
};

static int pte_off(struct mm_object_ref * or, struct mm_map *mm, void *d)
{
    struct sync_data *data = d;
    address_t *adp;
    unsigned flags;
    struct mm_map *sync_mm;
    off64_t sync_start;
    off64_t sync_end;
    off64_t start;
    off64_t end;
    uintptr_t start_vaddr;
    uintptr_t end_vaddr;

    adp = or->adp;
    flags = mm->mmap_flags;
    if (flags & data->bits) {
        sync_mm = data->mm;
        sync_start = sync_mm->offset;
        sync_end = sync_start + (off64_t) (sync_mm->end - sync_mm->start);
        start = mm->offset;
        end = start + (off64_t) (sync_mm->end - sync_mm->start);
        if (((start >= sync_start) && (start <= sync_end)) ||
            ((end >= sync_start) && (end <= sync_end))) {
            if (start < sync_start)
                start = sync_start;
            if (end > sync_end)
                end = sync_end;
            start_vaddr = mm->start + (uintptr_t) (start - mm->offset);
            end_vaddr = mm->start + (uintptr_t) (end - mm->offset);
            if (data->bits & PROT_READ) {
                pte_unmap(adp, start_vaddr, end_vaddr, or->obp);
            } else {
                (void) pte_prot(adp, start_vaddr, end_vaddr,
                                flags & ~data->bits, or->obp);
            }
        }
    }
    return EOK;
}


static int write_page(struct sync_data *data, void *buff)
{
    path_object_t *obp = data->mm->obj_ref->obp;
    off64_t off = data->off;
    off64_t len;
    int got;

    len = obp->mem.mm.size - off;
    if (len > QUANTUM_SIZE)
        len = QUANTUM_SIZE;

    got = (int) pwrite(obp->fdmem.fd, buff, (size_t) len, off);

    if ((got >= 0) && (got != (unsigned) len)) {
        errno = ENOSPC;
        got = -1;
    }
    return got;
}


static int do_write(void *dst, size_t len, void *d)
{
    struct sync_data *data = d;

#ifndef NDEBUG

    /* Things need to get more sophisticated if we write out more than a page at
     * a time
     */
    if (len != QUANTUM_SIZE)
        crash();
#endif

    data->got = write_page(data, dst);
    return EOK;
}


static int pmem_off(path_object_t *obp,
                    off64_t off,
                    struct pa_quantum *pq,
                    size_t num,
                    void *d)
{
    struct sync_data *data = d;
    struct mm_map *mm;
    uintptr_t vaddr;
    int got;
    unsigned i;
    unsigned bits;

    for (i = 0; i < num; ++i) {
        /* FUTURE: Gather up contiguous runs of PAQ_FLAG_MODIFIED FUTURE: and
         * write them out in one go.
         */
        bits = data->bits;
        if ((pa_pq_flags(pq) & bits) == PAQ_FLAG_MODIFIED) {
            mm = data->mm;
            vaddr = mm->start + (uintptr_t) (off - mm->offset);
            data->off = off;

            /* We can't use 'vaddr' directly to write out the page because we
             * can't guarantee that the page table is properly set up (consider
             * a second process does a msync() for the object followed by a
             * third touching the memory to turn the PAQ_FLAG_MODIFIED back on).
             */
            (void) pte_temp_map(mm->obj_ref->adp, vaddr, pq, mm, QUANTUM_SIZE,
                                do_write, data);
            got = data->got;
            if (got == -1)
                return errno;
        }
        pa_pq_and_not_flags(pq, bits);
        ++pq;
        off += QUANTUM_SIZE;
    }
    return EOK;
}


int mm_sync(path_object_t *obp, struct mm_map *mm, int flags)
{
    struct sync_data data;
    off64_t end;
    int r;

    data.mm = mm;
    end = mm->offset + (off64_t) (mm->end - mm->start);
    if (flags & (MS_SYNC | MS_ASYNC)) {
        data.bits = PROT_WRITE;
        memref_walk(obp, pte_off, &data);
        data.bits = PAQ_FLAG_MODIFIED;
        r = memobj_pmem_walk(MPW_SYSRAM, obp, mm->offset, end, pmem_off, &data);
        if (r != EOK)
            return r;
        if (flags & MS_SYNC) {
            if (fdatasync(obp->fdmem.fd) != 0) {
                return errno;
            }
        }
    }
    r = EOK;
    if (flags & MS_INVALIDATE) {
        data.bits = PROT_READ | PROT_WRITE | PROT_EXEC;
        memref_walk(obp, pte_off, &data);
        data.bits = PAQ_FLAG_INITIALIZED;
        r = memobj_pmem_walk(MPW_SYSRAM, obp, mm->offset, end, pmem_off, &data);
    }
    return r;
}


int as_msync(pid_t pid, process_t *prp, uintptr_t vaddr, size_t len, int flags)
{
    int r;
    struct map_set ms;
    struct mm_map *mm;
    struct mm_object_ref * or ;
    path_object_t *obp;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;

    /* MS_SYNC and MS_ASYNC are mutually exclusive */
    if ((flags & (MS_SYNC | MS_ASYNC)) == (MS_SYNC | MS_ASYNC))
        return EINVAL;

    if (!tmp)
        return EINVAL;
    r = map_isolate(&ms, &tmp->map, vaddr, len, MI_SPLIT);
    if (r != EOK)
        goto fail1;

    /* We have to make sure there are extra threads available when we go to
     * write a page out.
     */
    if (proc_thread_pool_reserve() != 0) {
        r = EAGAIN;
        goto fail2;
    }
    proc_lock_owner_mark(pid);

    if (flags & MS_INVALIDATE_ICACHE) {
        /* RISC-V needs an explicit fence.i on every hart after loader writes.
         * Skip the broadcast if no isolated mapping is executable — POSIX
         * scopes MS_INVALIDATE_ICACHE to PROT_EXEC regions, and a fence.i IPI
         * to every hart is not free.
         */
        for (mm = ms.first; mm != ms.last->next; mm = mm->next) {
            if (mm->mmap_flags & PROT_EXEC)
                break;
        }
        if (mm != ms.last->next) {
            r = kernel_icache_sync_all();
            if (r != EOK)
                goto fail3;
        }
    }

    if (!(flags & MS_CACHE_ONLY)) {
        for (mm = ms.first; mm != ms.last->next; mm = mm->next) {
            or = mm->obj_ref;
            if (or) {
                obp = or->obp;

                if ((flags & MS_INVALIDATE) ||
                    ((flags & (MS_SYNC | MS_ASYNC)) &&
                     !(mm->mmap_flags & MAP_NOSYNCFILE))) {
                    int has_lock = memobj_cond_lock(obp);
                    if ((obp->hdr.type == OBJECT_MEM_FD) &&
                        ((obp->mem.mm.flags & MM_FDMEM_NEEDSSYNC) ||
                         (flags & MS_INVALIDATE))) {
                        /* if the caller specified MS_INVALIDATE on a NOSYNCFILE
                         * mapping, skip the MS_SYNC/MS_ASYNC processing
                         */
                        int sync_flags = (mm->mmap_flags & MAP_NOSYNCFILE)
                                             ? flags & ~(MS_SYNC | MS_ASYNC)
                                             : flags;
                        r = mm_sync(obp, mm, sync_flags);
                    }
                    if (!has_lock)
                        memobj_unlock(obp);
                    if (r != EOK)
                        goto fail3;
                }
            }
        }
    }

fail3:
    proc_thread_pool_reserve_done();

fail2:
    map_coalese(&ms);

fail1:
    return r;
}
