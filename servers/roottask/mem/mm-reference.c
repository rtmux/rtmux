/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"
#include "root-process.h"


struct data {
    off64_t off_start;
    off64_t off_end;
    struct mm_map *mm;
    struct pa_quantum *pq;
    uintptr_t va;
    int unlock_obp;
    int icache_check;
    unsigned flags;
};

#define INIT_MPW_FLAGS MPW_SYSRAM /* Might be overridden - see pq_init(). */



/* Initialize the contents of a temporarily-mapped run of pages.
 *
 * Called from pte_temp_map with a kernel-direct-map VA covering one run of
 * physical quanta. Zeroes the pages for anonymous/shared/typed objects, or
 * reads file contents (zero-filling any trailing bss) for fd-backed objects.
 * Parameters:
 *   dst : kernel-direct-map VA of the run to initialize
 *   len : length of the run in bytes
 *   d   : pointer to the struct data describing the mapping
 * Return value: EOK on success, or an errno on failure.
 */
static int pq_init_run(void *dst, size_t len, void *d)
{
    struct data *data = d;
    struct mm_map *mm;
    struct pa_quantum *pq;
    path_object_t *obp;
    size_t got;
    unsigned last_page_bss; /* bounded by page size */
    uintptr_t end;
    uintptr_t real_end;

    pq = data->pq;
    mm = data->mm;
    obp = mm->obj_ref->obp;
    /* RUSH3: Make more object oriented... */
    switch (obp->hdr.type) {
    case OBJECT_MEM_TYPED:
    case OBJECT_MEM_ANON:
    case OBJECT_MEM_SHARED:
        if ((ssize_t) len <= 0 ||
            ((mm->mmap_flags & PROT_NOCACHE) && len > 0x1000000)) {
            root_log("pq_init_run: BOGUS len=%lx, aborting\n",
                     (unsigned long) len);
            return EINVAL;
        }

        /* dst is a kernel-direct-map VA returned by pte_temp_map (paddr +
         * KERNEL_VIRT_BASE). Roottask runs in U-mode with the heap mapped
         * PTE_U=0, so it cannot memset the kernel-direct-map directly; the
         * zeroing is routed through the kernel_page_zero kernel extension,
         * which runs in S-mode where the direct map is writable.
         */
        if (mm->mmap_flags & PROT_NOCACHE) {
            /* Chunk the zeroing: one kernel_page_zero kernel extension over the
             * full run (up to 16 MiB) would memset uncached RAM in S-mode with
             * preemption disabled for milliseconds, blowing the RT budget. 64
             * KiB chunks bound each non-preemptible burst.
             */
            paddr_t p = (paddr_t) ((uintptr_t) dst - CPU_1TO1_VADDR_BIAS);
            size_t left = len;

            while (left) {
                size_t chunk = left < (64 * 1024) ? left : (64 * 1024);
                (void) kernel_page_zero(p, chunk);
                p += chunk;
                left -= chunk;
            }
        } else {
            void *endp;

            endp = (uint8_t *) dst + len;
            do {
                if (!(pa_pq_flags(pq) & PAQ_FLAG_ZEROED)) {
                    (void) kernel_page_zero(
                        (paddr_t) ((uintptr_t) dst - CPU_1TO1_VADDR_BIAS),
                        QUANTUM_SIZE);

                    /* RUSH3: If we set PAQ_FLAG_ZEROED here and maintained
                     * RUSH3: it properly (e.g. turned it off when setting
                     * RUSH3: PAQ_FLAG_MODIFIED), we might be able save RUSH3:
                     * some zeroing when reusing the page. Gather RUSH3: some
                     * stats on how often a page is freed while RUSH3: still
                     * zeroed - don't forget to turn off RUSH3: PAQ_FLAG_ZEROED
                     * in the OBJECT_MEM_FD case.
                     */
                }
                ++pq;
                dst = (uint8_t *) dst + QUANTUM_SIZE;
            } while (dst < endp);
        }
        break;
    case OBJECT_MEM_FD:
        if (proc_thread_pool_reserve() != 0) {
            return EAGAIN;
        }
        end = data->va + len - 1;
        real_end = mm->end - mm->last_page_bss;
        if (end > real_end) {
            last_page_bss = (unsigned) (end - real_end);
            mm->last_page_bss -= (unsigned short) last_page_bss;
        } else {
            last_page_bss = 0;
        }
        size_t read_len = len - last_page_bss;
        got = (size_t) proc_read(obp->fdmem.fd, dst, (ssize_t) read_len,
                                 mm->offset + (off64_t) (data->va - mm->start));
        proc_thread_pool_reserve_done();
        if (got == (size_t) -1) {
            root_log("pq_init_run: proc_read(fd=%d) failed, errno=%d\n",
                     obp->fdmem.fd, errno);
            return errno;
        }

        /* Bound against the requested length, not len: a buggy/hostile fs
         * server returning more than asked (but <= len) would otherwise let
         * file data spill into the trailing bss that must be zero-filled.
         */
        if (got > read_len) {
            root_log("pq_init_run: proc_read returned %lx > %lx (fd=%d)\n",
                     (unsigned long) got, (unsigned long) read_len,
                     obp->fdmem.fd);
            return EIO;
        }
        if ((len != got) && !(pa_pq_flags(pq) & PAQ_FLAG_ZEROED)) {
            /* Partial last page — same kernel-direct-map issue as the ANON path
             * above; route through kernel_page_zero.
             */
            uintptr_t pad = (uintptr_t) dst + got;
            (void) kernel_page_zero((paddr_t) (pad - CPU_1TO1_VADDR_BIAS),
                                    len - got);
        }
        data->icache_check = 1;
        break;
    default:
        crash();
    }

    data->va += len;
    return EOK;
}


/* RUSH1: Have to be more careful here. If one guy has the page RUSH1: mapped
 * MAP_NOINIT and another doesn't, we have a race RUSH1: condition about whether
 * the page is inited or not. RUSH1: Can only do the MAP_NOINIT if _all_ mmap's
 * to this RUSH1: page are MAP_NOINIT.
 */
#define INIT_REQUIRED(mmf, pq)                    \
    (!(pa_pq_flags(pq) & PAQ_FLAG_INITIALIZED) && \
     ((pa_pq_flags(pq) & PAQ_FLAG_INIT_REQUIRED) || !((mmf) & MAP_NOINIT)))


/* pmem-walk callback that initializes runs of pages needing init.
 *
 * Skips pages already initialized or not requiring init, then maps and
 * initializes each contiguous run that does, marking the quanta
 * PAQ_FLAG_INITIALIZED. Parameters:
 *   obp : object being walked (unused here)
 *   off : object offset of this quantum range (unused here)
 *   pq  : first quantum of the range
 *   num : number of quanta in the range
 *   d   : pointer to the struct data describing the mapping
 * Return value: EOK on success, or an errno on failure.
 */
static int pq_init(path_object_t *obp,
                   off64_t off,
                   struct pa_quantum *pq,
                   size_t num,
                   void *d)
{
    struct data *data = d;
    int r;
    struct mm_map *mm;
    size_t i;
    size_t j;

#ifndef NDEBUG
    /* Check to make sure we've allocated pmem for the whole range */
    if (!pq)
        crash();
#undef INIT_MPW_FLAGS
#define INIT_MPW_FLAGS (MPW_HOLES | MPW_SYSRAM)
#endif
    mm = data->mm;

    for (;;) {
        /* skip over pages that don't need initialization */
        for (;;) {
            if (num == 0)
                return EOK;
            if (INIT_REQUIRED(mm->mmap_flags, pq))
                break;
            if (!(pa_pq_flags(pq) & PAQ_FLAG_INITIALIZED)) {
                (void) pa_pq_modify_flags(pq, PAQ_FLAG_INITIALIZED,
                                          PAQ_FLAG_INIT_REQUIRED);
            }
            ++pq;
            data->va += QUANTUM_SIZE;
            --num;
        }
        /* find the run of pages that need initialization; */
        data->pq = pq;
        i = 0;
        for (;;) {
            ++pq;
            ++i;
            if (i >= num)
                break;
            if (!INIT_REQUIRED(mm->mmap_flags, pq))
                break;
        }
        /* do the initialization */
        r = pte_temp_map(mm->obj_ref->adp, data->va, data->pq, mm,
                         i << QUANTUM_BITS, pq_init_run, d);
        if (r != EOK)
            return r;
        pq = data->pq;
        for (j = 0; j < i; ++j, ++pq) {
            (void) pa_pq_modify_flags(pq, PAQ_FLAG_INITIALIZED,
                                      PAQ_FLAG_INIT_REQUIRED);
        }
        num -= i;
    }
}


/* pmem-walk callback that installs page-table entries for a run.
 *
 * For fake (direct-physical) quanta it maps the whole run in one operation,
 * handling write-fault deferral and cache-coherency setup. For real quanta it
 * maps runs of initialized pages, adjusting protection for copy-on-write and
 * flushing caches when needed for instruction coherency. Parameters:
 *   obp : object being walked
 *   off : object offset of this quantum range
 *   pq  : first quantum of the range
 *   num : number of quanta in the range
 *   d   : pointer to the struct data describing the mapping
 * Return value: EOK on success, or an errno on failure.
 */
static int pq_map(path_object_t *obp,
                  off64_t off,
                  struct pa_quantum *pq,
                  size_t num,
                  void *d)
{
    struct data *data = d;
    struct mm_map *mm;
    struct mm_object_ref * or ;
    uintptr_t start;
    uintptr_t curr;
    paddr_t paddr;
    off64_t skip;
    unsigned mmap_flags;
    int r;
    unsigned i;
    unsigned paq_flag_intersection;
    unsigned mask;
    unsigned map_type;
    int need_dcache_flush_for_icache;

    mm = data->mm;
    or = mm->obj_ref;
    mmap_flags = mm->mmap_flags;
    map_type = mmap_flags & MAP_TYPE;
    start = data->va;
    paddr = pa_quantum_to_paddr(pq);
    need_dcache_flush_for_icache = 0;
    if (pa_pq_blk(pq) == PAQ_BLK_FAKE) {
        /* We only have a single pa_quantum_fake to cover the whole of the run,
         * so we need to adjust the paddr with the offset from the start.
         */
        skip = 0;
        if (data->off_start > off) {
            skip = data->off_start - off;
            paddr += (uint64_t) skip;
            num -= LEN_TO_NQUANTUM(skip);
        }
        if ((map_type != MAP_SHARED) && !(data->flags & MR_WRITE)) {
            /* Still want to fault on first write */
            mmap_flags &= ~(unsigned) PROT_WRITE;
        }
        if (!(mm->extra_flags & EXTRA_FLAG_CACHE_CLEANED)) {
            mm->extra_flags |= EXTRA_FLAG_CACHE_CLEANED;

            /* We're making an assumption here that we set up the PTE's for a
             * direct mapping in only one operation and as such we can get away
             * with just maintaining one bit about the cache being invalidated
             * rather than a bitmask for each of the pages. This is currently
             * the way things work because of the gear in mmap.c
             * (setup_mappings()). If things ever change, the check below will
             * catch the problem and someone will read this comment and say
             * Aaarrrggghhh!
             */
            if ((((mm->end - mm->start) + 1) != NQUANTUM_TO_LEN(num)) &&
                (obp->hdr.type == OBJECT_MEM_ANON)) {
                crash();
            }

            /* RUSH3: We can optimize this by not turning off the PROT_NOCACHE
             * RUSH3: if the paddr doesn't point to RAM: pa_paddr_to_quantum()
             * RUSH3: returns NULL. Maybe not - what about special purpose
             * RUSH3: ram that's outside the sysram region(s)? We need to map
             * cached initially so we can flush
             */
            mmap_flags &= ~(unsigned) PROT_NOCACHE;
        }

        data->va = start + NQUANTUM_TO_LEN(num);
        r = pte_map(or->adp, start, data->va - 1, (int) mmap_flags, or->obp,
                    paddr, 0);
        if (r == EOK) {
            if (mmap_flags & PROT_WRITE) {
                pa_pq_or_flags(pq, PAQ_FLAG_MODIFIED);
            }
            if (!(mmap_flags & PROT_NOCACHE) &&
                (mm->mmap_flags & PROT_NOCACHE)) {
                /* We've done a cached mapping, but the actual flags were for
                 * no-cache. We did this so that we can make sure there are no
                 * bits sitting around in the cache for the physical memory by
                 * invalidating the cache first
                 */
                r = pte_map(or->adp, start, data->va - 1,
                            (int) (mmap_flags | PROT_NOCACHE), or->obp, paddr,
                            0);
            }
        }
    } else {
        data->va = start + NQUANTUM_TO_LEN(num);
        curr = start;
        for (;;) {
            /* Skip over non-initialized regions */
            for (;;) {
                if (num == 0)
                    return EOK;
                if (pa_pq_flags(pq) & PAQ_FLAG_INITIALIZED)
                    break;
                if (mmap_flags & PROT_EXEC) {
                    pa_pq_or_flags(pq, PAQ_FLAG_HAS_INSTRS);
                }
                ++pq;
                curr += QUANTUM_SIZE;
                paddr += QUANTUM_SIZE;
                --num;
            }
            paq_flag_intersection = ~0U;
            start = curr;
            /* Find run of init'd pages */
            i = 0;
            for (;;) {
                unsigned cur_flags = pa_pq_flags(pq);
                paq_flag_intersection &= cur_flags;
                if (data->flags & MR_WRITE) {
                    pa_pq_or_flags(pq, PAQ_FLAG_MODIFIED);
                }
                if (!(mmap_flags & PROT_EXEC) && data->icache_check &&
                    (cur_flags & PAQ_FLAG_HAS_INSTRS)) {
                    pa_pq_and_not_flags(pq, PAQ_FLAG_HAS_INSTRS);
                    need_dcache_flush_for_icache = 1;
                }
                curr += QUANTUM_SIZE;
                ++pq;
                ++i;
                if (i == num)
                    break;
                if (!(pa_pq_flags(pq) & PAQ_FLAG_INITIALIZED))
                    break;
            }
            if (data->flags & MR_WRITE) {
                mask = ~0U;
            } else if ((paq_flag_intersection & PAQ_FLAG_MODIFIED) &&
                       (map_type == MAP_SHARED)) {
                /* If MAP_SHARED and all the pages were already marked as
                 * modified, we can turn on write perms in the PTE, even if this
                 * reference is only a read request.
                 */
                mask = ~0U;
            } else {
                /* Still need to know about the first write to the pages. */
                mask = ~(unsigned) PROT_WRITE;
            }
            /* Map the initialized region */
            r = pte_map(or->adp, start, curr - 1, (int) (mmap_flags & mask),
                        or->obp, paddr, 0);
            if (r != EOK)
                break;
            paddr += i << QUANTUM_BITS;
            num -= i;
            if (data->icache_check) {
                unsigned cache_flags = 0;

                if (mm->mmap_flags & PROT_EXEC) {
                    cache_flags = MS_INVALIDATE_ICACHE;
                } else if (need_dcache_flush_for_icache) {
                    /* We don't have PROT_EXEC perms for the mapping, but
                     * there's at least one page that has a different mapping
                     * that does have PROT_EXEC and wasn't initialized at the
                     * time of the mmap(). We need to make sure that we flush
                     * the page down to a unified cache so the other mapping
                     * will pick up the right data for instruction fetches
                     */
                    cache_flags = MS_ASYNC;
                }
                if (cache_flags != 0) {
                    /* Program text (or data that aliases an executable mapping)
                     * was just written into these — possibly recycled — pages
                     * via ordinary data stores. RISC-V keeps NO coherence
                     * between data stores and the I-cache, and has no user-mode
                     * cross-hart fence.i, so before this code can run on any
                     * hart, fence.i must execute there. A hart still holding
                     * the previous tenant's I-cache lines for these physical
                     * pages would otherwise fetch stale instructions and jump
                     * into garbage (the FU740 "any program faults after N runs"
                     * bug — invisible on QEMU, which models no I-cache).
                     *
                     * Broadcast fence.i to every hart now, while we still hold
                     * the as, so the program is safe to dispatch wherever the
                     * scheduler later picks. Routed through a kernel extension:
                     * U-mode roottask cannot issue fence.i / SBI.
                     *
                     * RUSH1: a narrow window remains between pte_map above and
                     * this sync where a peer already executing in this as could
                     * fetch stale lines; closing it fully needs the pages
                     * mapped kernel-only until after the sync. Broadcasting
                     * here covers the spawn case (the program is not yet
                     * dispatched) which is what bites.
                     */
                    (void) kernel_icache_sync_all();
                }
            }
        }
    }
    return r;
}


/* Copy a run of source pages into freshly-mapped private storage.
 *
 * Copies from the target process's user VA into the kernel-direct-map VA of the
 * new anonymous pages, zero-filling any trailing bss. Parameters:
 *   dst : kernel-direct-map VA of the destination run
 *   len : length of the run in bytes
 *   d   : pointer to the struct data describing the mapping
 * Return value: EOK on success, or an errno on failure.
 */
static int pq_copy_run(void *dst, size_t len, void *d)
{
    struct data *data = d;
    struct mm_map *mm;
    size_t last_page_bss;
    uintptr_t src;
    pid_t target_pid;
    int r;

    mm = data->mm;
    last_page_bss = mm->last_page_bss;
    src = data->va;
    data->va += len;
    if ((data->va - 1) > (mm->end - last_page_bss)) {
        len -= last_page_bss;
        memset((char *) dst + len, 0, last_page_bss);
    }

    /* src is a user VA in the TARGET process's as. In U-mode roottask the satp
     * stays loaded with roottask's own pgdir (the ROOTTASK_PID carve-out in
     * kernel_exit), so a direct memcpy from "src" would fault. as_memcpy
     * switches satp on the kernel side, copies under xfer fault recovery, then
     * restores satp. dst here is a kernel-direct-map VA from pte_temp_map(),
     * which remains accessible across the satp swap.
     */
    target_pid = mm->obj_ref ? mm->obj_ref->pid : 0;
    r = as_memcpy(target_pid, (void *) src, dst, len, AS_MEMCPY_FROM_TARGET);
    return r ? r : EOK;
}


/* pmem-walk callback that copies a run into private storage.
 *
 * Temporarily maps the run and copies via pq_copy_run, then marks the quanta
 * PAQ_FLAG_INITIALIZED. Parameters:
 *   obp : object being walked (unused here)
 *   off : object offset of this quantum range (unused here)
 *   pq  : first quantum of the range
 *   num : number of quanta in the range
 *   d   : pointer to the struct data describing the mapping
 * Return value: EOK on success, or an errno on failure.
 */
static int pq_copy(path_object_t *obp,
                   off64_t off,
                   struct pa_quantum *pq,
                   size_t num,
                   void *d)
{
    struct data *data = d;
    struct mm_map *mm;
    int r;

    mm = data->mm;
    {
        r = pte_temp_map(mm->obj_ref->adp, data->va, pq, mm,
                         NQUANTUM_TO_LEN(num), pq_copy_run, d);
        if (r == EOK) {
            do {
                pa_pq_or_flags(pq, PAQ_FLAG_INITIALIZED);
                ++pq;
                --num;
            } while (num > 0);
        }
    }
    return r;
}


/* Convert a shared/file-backed mapping run to private anonymous memory.
 *
 * Maps the source read-only, allocates anonymous backing store, copies the
 * contents into it, then swaps the map structures over to the new anonymous
 * object (copy-on-write fault handling). Parameters:
 *   data : the struct data describing the mapping; on success data->mm
 *          is updated to the new anonymous mapping
 * Return value: EOK on success, or an errno on failure.
 */
static int privatize(struct data *data)
{
    struct map_set ms;
    struct map_set repl_ms;
    int r;
    uintptr_t start;
    struct mm_object_ref * or ;
    path_object_t *anon;
    path_object_t *obp;
    struct mm_map *mm;
    struct mm_map *new;
    off64_t off;
    size_t len;
    address_t *adp;
    unsigned orig_mmap_flags;

    start = data->va;
    mm = data->mm;
    or = mm->obj_ref;
    adp = or->adp;
    obp = or->obp;


    /* Create a readable mapping. We turn off icache_check so that pq_map()
     * doesn't bother with any cache flushing, since we're not going to actually
     * be executing from this storage. It gets turned on at the end of this
     * function. We also turn of MR_WRITE since we're not actually modifying
     * these pages We temporarily set PROT_READ to ensure that the mapping is
     * readable. eg. if the mmap() specified only PROT_WRITE, the pq_map() will
     * clear the PROT_WRITE to catch the first write, resulting in a crash in
     * the pq_copy() below because the mapping will have no access permissions.
     */
    data->icache_check = 0;
    data->flags &= ~(unsigned) MR_WRITE;
    orig_mmap_flags = mm->mmap_flags;
    mm->mmap_flags |= PROT_READ;
    r = memobj_pmem_walk(MPW_PHYS | MPW_SYSRAM, obp, data->off_start,
                         data->off_end, pq_map, data);
    mm->mmap_flags = orig_mmap_flags;
    if (r != EOK)
        goto fail1;
    data->flags |= MR_WRITE;

    /* Allocate anonymous memory.  adp->anon moved to root_process_t. */
    anon = (or->pid != 0) ? (root_proc(or->pid))->anon : NULL;
    if (!anon)
        goto fail1;
    if (ANMEM_MULTI_REFS(anon)) {
        memobj_lock(anon);
    }
    len = (size_t) ((data->off_end - data->off_start) + 1);

    /* RUSH2: Assuming that we always get the same offset for the same 'start'
     */
    off = anmem_offset(anon, start, len);
    /* Copy from the shared to private storage */
    data->va = start;
    data->off_start = off;
    data->off_end = off + (off64_t) len - 1;
    memobj_pmem_walk(MPW_SYSRAM, anon, off, data->off_end, pq_copy, data);
    /* Switch the map structures to the anonymous memory */
    r = map_create(&ms, &repl_ms,
                   (or->pid != 0) ? &(root_proc(or->pid))->map : NULL, start,
                   len, 0, MAP_FIXED);
    if (r != EOK)
        goto fail3;
    new = ms.first;
    new->offset = off;

    /* Keep MAP_ELF on privatized mappings. Loader paths may mprotect unaligned
     * PLT regions, and vm-ctrl.c relies on MAP_ELF to tolerate that case.
     */
    new->mmap_flags =
        (mm->mmap_flags & ~(unsigned) (MAP_PHYS | MAP_NOSYNCFILE)) |
        MAP_SYSRAM | MAP_ANON;
    new->extra_flags = mm->extra_flags;
    new->reloc = mm->reloc;

    /* MAPFIELDS: copy field data from 'mm' to 'new' The as owner's pid lives on
     * or->pid (mm_object_ref); pass it so ms_unmap's proc_lock_owner_mark /
     * rwlock checks see the correct root_process_t. The rwlock state is
     * per-root_process_t, so a pid=0 fallback would trip the !WLOCK CRASHCHECK
     * in proc_lock_owner_mark.
     */
    r = ms_unmap(or->pid, adp, &repl_ms, UNMAP_PRIVATIZE | UNMAP_NORLIMIT);
    if (r != EOK)
        goto fail4;
    r = map_add(&ms);
    if (r != EOK)
        goto fail4;
    r = memref_add(new, anon, mm->obj_ref ? mm->obj_ref->pid : 0, adp, NOFD);
    if (r != EOK)
        goto fail5;
    map_coalese(&ms);

    data->mm = ms.first;
    data->unlock_obp = ANMEM_MULTI_REFS(anon);
    data->icache_check = 1;
    return EOK;

fail5:
    map_remove(&ms);

fail4:
    map_destroy(&ms);
    map_coalese(&repl_ms);

fail3:
    if (ANMEM_MULTI_REFS(anon)) {
        memobj_unlock(anon);
    }

fail1:
    return r;
}


/* Reference (fault in) a range of a single mapping.
 *
 * Allocates and initializes the backing physical memory for [start, end],
 * privatizing the range when a write reference hits a non-anonymous private
 * mapping, then installs the page-table entries. Operates on one mm_map entry
 * at a time. Parameters:
 *   mmp   : in/out — the mapping to reference; updated if privatize()
 *           replaces it
 *   start : start virtual address of the range
 *   end   : end virtual address of the range
 *   flags : MR_* reference flags (MR_WRITE, MR_NOINIT, MR_TRUNC, …)
 *   ms    : map set to keep in sync across a privatize (may be NULL)
 * Return value: EOK on success, or an errno on failure.
 */
int memory_reference(struct mm_map **mmp,
                     uintptr_t start,
                     uintptr_t end,
                     unsigned flags,
                     struct map_set *ms)
{
    path_object_t *obp;
    struct mm_map *mm;
    struct mm_object_ref * or ;
    int r;
    struct data data;
    uintptr_t valid_end;

    mm = *mmp;

    /* Memory-pressure policy (section 18): RTMux does not reclaim, page out, or
     * swap a process's mapped memory, and has no OOM killer, so a process keeps
     * its mapped memory until it frees it or exits. The RLIMIT_AS check at map
     * time (rlimit_blown in mm-rlimit.c) is currently a stub that always
     * passes, so no per-process address-space or resident cap is actually
     * enforced. A future policy could enforce that check or add a resident
     * budget; realtime processes stay safe by allocating their working set up
     * front, since it is never reclaimed.
     */

    /* Only handles one mm_map entry at a time. */
    CRASHCHECK(start < mm->start || end > mm->end);

    or = mm->obj_ref;
    obp = or->obp;

    if (mm->mmap_flags & PROT_WRITE) {
        if (!CPU_FAULT_ON_WRITE_WORKS) {
            /* If we can write to this memory and fault on write doesn't work,
             * always pretend the access is a write to force privatization (if
             * needed)
             */
            flags |= MR_WRITE;

            /* RUSH3: If we force on MR_WRITE and MR_NOINIT is also on, RUSH3:
             * kick out of the optimization because privatize() doesn't RUSH3:
             * handle a range that has uninitialized quantums properly.
             */
            if (((mm->mmap_flags & MAP_TYPE) == MAP_PRIVATE) &&
                (obp->hdr.type != OBJECT_MEM_ANON) && (flags & MR_NOINIT)) {
                return EOK;
            }
        }
    }

    /* RUSH1: We might be asked to 'reference' beyond the end of a shared RUSH1:
     * memory object (the start/end pointers might be beyond RUSH1:
     * obp->mem.mm.size). Make sure we handle that without blowing RUSH1: up.
     */
    start = ADDR_PAGE(start);
    end = ADDR_PAGE(end) | (__PAGESIZE - 1);
    data.off_end = mm->offset + (off64_t) (end - mm->start);
    data.off_start = mm->offset + (off64_t) (start - mm->start);
    if (flags & MR_TRUNC) {
        /* truncate reference to the limit of the object */
        if (data.off_start >= obp->mem.mm.size) {
            /* first reference is past end of object - user will fault if he
             * ends up touching this location
             */
            return EOK;
        }
        if (data.off_end >= obp->mem.mm.size) {
            data.off_end = (obp->mem.mm.size - 1) | (off64_t) (__PAGESIZE - 1);
        }
    }

    data.mm = mm;
    data.unlock_obp = 0;
    data.icache_check = 0;
    data.flags = flags;

    valid_end = mm->end - mm->last_page_bss;

    if (!(flags & MR_NOINIT)) {
        data.va = start;
        r = memobj_pmem_walk(INIT_MPW_FLAGS, obp, data.off_start, data.off_end,
                             pq_init, &data);
        if (r != EOK)
            return r;
        if (end > valid_end) {
            /* Force privatization. We can only zero the last partial page when
             * it's been privatized, since the memory might be coming from an
             * OBJECT_MEM_SHARED and we don't want to damage the underlying
             * memory.
             */
            flags |= MR_WRITE;
        }
    } else if (end > valid_end) {
        /* Don't try to map in the partial last page - we need to wait until we
         * really reference it so it can be privatized
         */
        if ((data.off_end - data.off_start) <= QUANTUM_SIZE) {
            return EOK;
        }
        data.off_end -= QUANTUM_SIZE;
    }

    /* RUSH2: Have to handle COW, COR */
    if (flags & MR_WRITE) {
        /* MR_WRITE might have been turned on after first assignment above */
        data.flags = flags;
        if ((mm->mmap_flags & MAP_TYPE) == MAP_PRIVATE) {
            switch (obp->hdr.type) {
            case OBJECT_MEM_ANON:
                break;
            default:
                /* Change the memory to private and anonymous. */
                data.va = start;
                r = privatize(&data);
                if (r != EOK)
                    return r;
                if (mm != data.mm) {
                    if (ms) {
                        map_set_update(ms, mm, data.mm);
                    }
                    mm = data.mm;
                    *mmp = mm;
                    or = mm->obj_ref;
                    obp = or->obp;
                }
                break;
            }
        } else if (obp->hdr.type == OBJECT_MEM_FD) {
            if (!(mm->mmap_flags & MAP_NOSYNCFILE)) {
                obp->mem.mm.flags |= MM_FDMEM_NEEDSSYNC;
            }
        }
    }

    switch (obp->hdr.type) {
    case OBJECT_MEM_SHARED:
        if (obp->mem.mm.flags & MM_SHMEM_IFS)
            break;
        /* fall through */
    case OBJECT_MEM_FD:
    case OBJECT_MEM_TYPED:
        /* RUSH3: If we could tell if the object has been written to RUSH3: or
         * not since the last PROC_EXEC mapping, we could RUSH3: reduce the
         * number of times that we have to invalidate RUSH3: the icache.
         */
        data.icache_check = 1;
        break;
    default:
        break;
    }

    data.va = start;
    r = memobj_pmem_walk(MPW_PHYS | MPW_SYSRAM, obp, data.off_start,
                         data.off_end, pq_map, &data);
    if (r != EOK)
        goto fail1;

fail1:
    if (data.unlock_obp)
        memobj_unlock(obp);
    return r;
}
