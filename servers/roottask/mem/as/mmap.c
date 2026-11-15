/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"
#include "root-process.h"

/* MAP_ELF may cover BSS beyond the file-backed portion of a segment. Keep the
 * zero-fill behavior here so both the main loader and dynamic loader use the
 * same VM path.
 */

/* Verify a typed-memory offset range lies within a restriction list.
 *
 * Parameters:
 *   off  : start offset
 *   size : range size in bytes
 *   list : restriction list to check against
 * Return value: EOK if fully covered, ENXIO otherwise.
 */
static int tymem_valid_offset(off64_t off,
                              size_t size,
                              struct pa_restrict *list)
{
    struct pa_restrict *rp;
    int found;
    off64_t end = off + (off64_t) size - 1;

    if (off < end) {
        do {
            found = 0;
            for (rp = list; rp; rp = rp->next) {
                if ((off >= (off64_t) rp->start) &&
                    (off <= (off64_t) rp->end)) {
                    off = (off64_t) rp->end + 1;
                    found = 1;
                }
                if ((end >= (off64_t) rp->start) &&
                    (end <= (off64_t) rp->end)) {
                    end = (off64_t) rp->start - 1;
                    found = 1;
                }
                if (end < off)
                    return EOK;
            }
        } while (found);
    }
    return ENXIO;
}


struct tymem_pmem {
    off64_t off;
    size_t end_quanta;
    int covering;
    struct pa_quantum *pq;
    struct pa_quantum **owner;
};

/* pmem-walk callback that creates fake quanta to cover allocatable holes.
 *
 * For each hole in the typed-memory object, allocates a fake pa_quantum and
 * links it onto the data->owner list; frees the partial list on allocation
 * failure. Parameters:
 *   obp : object being walked (unused here)
 *   off : object offset of the hole (encoded as the fake paddr)
 *   pq  : current quantum (unused here)
 *   num : number of quanta the hole spans
 *   d   : pointer to the struct tymem_pmem accumulator
 * Return value: EOK on success, ENOMEM on allocation failure.
 */
static int tymem_fakes(path_object_t *obp,
                       off64_t off,
                       struct pa_quantum *pq,
                       size_t num,
                       void *d)
{
    struct tymem_pmem *data = d;
    struct pa_quantum *next;

    data->covering = 1;
    pq = pa_alloc_fake((paddr_t) off, NQUANTUM_TO_LEN(num));
    if (!pq) {
        for (pq = data->pq; pq; pq = next) {
            next = pq->u.inuse.next;
            pa_free_fake(pq);
        }
        return ENOMEM;
    }
    *data->owner = pq;
    data->owner = &pq->u.inuse.next;
    return EOK;
}

/* pmem-walk callback that splits a fake quantum at a typed-memory boundary.
 *
 * Splits the quantum so that the allocated region begins on a quantum boundary,
 * when the requested offset falls inside an existing entry. Parameters:
 *   obp : object being walked
 *   off : object offset of this quantum
 *   pq  : current quantum (unused here)
 *   num : number of quanta in the range
 *   d   : pointer to the struct tymem_pmem describing the request
 * Return value: EOK if no split needed or split succeeded, else errno.
 */
static int tymem_split(path_object_t *obp,
                       off64_t off,
                       struct pa_quantum *pq,
                       size_t num,
                       void *d)
{
    struct tymem_pmem *data = d;
    size_t end_quanta;

    end_quanta = LEN_TO_NQUANTUM(off) + num;
    if ((data->off <= off) || (data->end_quanta >= end_quanta)) {
        return EOK;
    }
    /* need to split */
    return memobj_pmem_split(obp, pq,
                             (unsigned) LEN_TO_NQUANTUM(data->off - off));
}

/* Allocate the physical backing for a typed-memory mapping.
 *
 * Depending on <flags>, allocates from the pool / resource database
 * (IMAP_TYMEM_ALLOCATE[_CONTIG]), creates fake quanta for an allocatable
 * mapping (IMAP_TYMEM_MAP_ALLOCATABLE), or claims a given physical range.
 * Splits the map set and adds the resulting quanta to the object's pmem list,
 * backing out cleanly on failure. Parameters:
 *   prp   : target process (kernel-side handle)
 *   obp   : typed-memory object
 *   ms    : map set covering the mapping (may be split)
 *   mm    : the mapping entry within <ms>
 *   flags : mmap / IMAP_TYMEM_* flags
 *   boff  : object offset of the mapping
 *   size  : size of the mapping in bytes
 * Return value: EOK on success, or an errno on failure.
 */
static int tymem_pmem_alloc(process_t *prp,
                            path_object_t *obp,
                            struct map_set *ms,
                            struct mm_map *mm,
                            unsigned flags,
                            off64_t boff,
                            size_t size)
{
    int r;
    struct pa_quantum *pq_add;
    struct pa_quantum *pq_next;
    struct pa_quantum *pq;
    struct mm_map *mm_add;
    unsigned pg_offset;
    struct tymem_pmem data;
    unsigned pa_flags;
    size_t split_point;
    size_t run_len;

    if (flags & MM_MAP_BELOW16M)
        return EINVAL;
    pg_offset = ADDR_OFFSET(boff);
    boff -= pg_offset;
    size += pg_offset;
    if (flags & (IMAP_TYMEM_ALLOCATE | IMAP_TYMEM_ALLOCATE_CONTIG)) {
        if ((boff != 0) || (pg_offset != 0))
            return EINVAL;
        pa_flags = 0;
        if (flags & IMAP_TYMEM_ALLOCATE_CONTIG)
            pa_flags = PAA_FLAG_CONTIG;
        /* RUSH2: Alignment, color.... */
        if (obp->mem.mm.flags & MM_MEM_RDB) {
            pq = tymem_rdb_alloc(obp, size, pa_flags);
        } else {
            pq = pa_alloc(size, 0, PAQ_COLOUR_NONE, pa_flags, NULL,
                          obp->mem.mm.restriction, 0);
        }
        if (!pq)
            return ENOMEM;
    } else if (flags & IMAP_TYMEM_MAP_ALLOCATABLE) {
        r = tymem_valid_offset(boff, size, obp->mem.mm.restriction);
        if (r != EOK)
            return r;
        data.pq = NULL;
        data.owner = &data.pq;

        /* Create fake quantums for all the regions that don't already have pmem
         * entries.
         */
        r = memobj_pmem_walk(MPW_HOLES, obp, boff, boff + (off64_t) size - 1,
                             tymem_fakes, &data);
        if (r != EOK)
            return r;
        pq = data.pq;
        mm->obj_ref = (void *) pq;
        mm->offset = boff;
    } else {
        r = tymem_valid_offset(boff, size, obp->mem.mm.restriction);
        if (r != EOK)
            return r;
        if (obp->mem.mm.flags & MM_MEM_RDB) {
            int tmp;

            pq = tymem_rdb_alloc_given(obp, (paddr_t) boff, size, &tmp);
            r = tmp;
        } else {
            pq = pa_paddr_to_quantum((paddr_t) boff);
            if (!pq)
                return ENXIO;
            r = pa_alloc_given(pq, LEN_TO_NQUANTUM(size), NULL);
        }
        if (r != EOK)
            return r;
    }
    if ((pa_pq_blk(pq) != PAQ_BLK_FAKE) || (pa_pq_flags(pq) & PAQ_FLAG_RDB)) {
        /* We're hiding the quantum pointer for the mapping in the 'obj_ref'
         * field in this section. It'll all be cleaned up before we leave.
         */

        split_point = mm->start - ms->first->start;
        mm_add = mm;
        for (pq_add = pq; pq_add; pq_add = pq_next) {
            pq_next = pa_pq_inuse_next(pq_add);
            mm_add->obj_ref = (void *) pq_add;
            mm_add->offset = (off64_t) pa_quantum_to_paddr(pq_add);
            data.off = mm_add->offset;
            data.end_quanta = LEN_TO_NQUANTUM((paddr_t) data.off) +
                              (unsigned) pa_pq_run(pq_add);
            run_len = NQUANTUM_TO_LEN(pa_pq_run(pq_add));
            if (data.off + (off64_t) run_len > obp->mem.mm.size) {
                obp->mem.mm.size = data.off + (off64_t) run_len;
            }
            data.covering = 0;

            /* Since sysram quantums override fake ones, we're going to delete
             * all the regions covered by fake entries when we get around to
             * adding the memory to the pmem list down below. Now, we might have
             * to allocate memory to delete from the middle of a fake entry,
             * which means that we might run out in the middle of adding, which
             * would be really obnoxious to back out of. Instead, we'll run
             * through and split the problematic fake entries up ahead of time.
             * That way, if we're going to run out of memory, we'll run out here
             * where it's easier to undo things.
             */
            r = memobj_pmem_walk(MPW_PHYS, obp, mm->offset,
                                 mm->offset + (off64_t) run_len - 1,
                                 tymem_split, &data);
            if (r != EOK)
                goto fail1;

            /* Keep track of the fact that we need to clean the region before
             * adding the new pmem entries.
             */
            mm_add->reloc = data.covering;
            if (pq_next) {
                split_point += run_len;
                r = map_split(ms, split_point);
                if (r != EOK)
                    goto fail1;
                mm_add = mm_add->next;
            }
            pa_pq_set_inuse_next(pq_add, NULL);
        }
    }
    for (;;) {
        pq_add = (void *) mm->obj_ref;
        mm->obj_ref = NULL;
        while (pq_add) {
            off64_t add_off;

            pq_next = pa_pq_inuse_next(pq_add);
            add_off = (off64_t) pa_quantum_to_paddr(pq_add);
            if (mm->reloc) {
                mm->reloc = 0;
                memobj_pmem_del_len(obp, add_off,
                                    NQUANTUM_TO_LEN(pa_pq_run(pq_add)));
            }
            (void) memobj_pmem_add_pq(obp, add_off, pq_add);
            pq_add = pq_next;
        }
        if (mm == ms->last)
            break;
        mm = mm->next;
    }
    /* RUSH3: It'd be nice to collapse adjacent, contiguous fake quantums */
    return EOK;

fail1:
    for (;;) {
        pq_add = (void *) mm->obj_ref;
        mm->obj_ref = NULL;
        while (pq_add) {
            pq_next = pa_pq_inuse_next(pq_add);
            if (pa_pq_blk(pq_add) == PAQ_BLK_FAKE) {
                if (pa_pq_flags(pq_add) & PAQ_FLAG_RDB) {
                    tymem_rdb_free(obp, pa_quantum_to_paddr(pq_add),
                                   NQUANTUM_TO_LEN(pa_pq_run(pq_add)));
                }
                pa_free_fake(pq_add);
            } else {
                pa_free(pq_add, (size_t) pa_pq_run(pq_add), 0);
            }
            pq_add = pq_next;
        }
        if (mm == ms->last)
            break;
        mm = mm->next;
    }
    return r;
}


/* Fault in (reference) the pages of a freshly-created mapping.
 *
 * For stacks, references the preload region honouring RLIMIT_STACK. Otherwise
 * walks each mapping entry, referencing the preload portion and pre-allocating
 * page tables for the remainder (direct physical mappings are referenced in
 * full). Parameters:
 *   pid     : target process pid
 *   prp     : target process (kernel-side handle)
 *   ms      : map set describing the new mapping
 *   flags   : mmap flags
 *   preload : number of bytes to fault in up front
 * Return value: EOK on success, or an errno on failure.
 */
static int rdecl setup_mappings(pid_t pid,
                                process_t *prp,
                                struct map_set *ms,
                                unsigned flags,
                                size_t preload)
{
    address_t *adp;
    root_process_t *tmp = root_proc(pid);
    size_t maxsize;
    size_t todo;
    uintptr_t start;
    uintptr_t end;
    int r;
    struct mm_object_ref * or ;
    path_object_t *obp;
    unsigned mr_flags;
    struct mm_map *mm;

    preload = ROUNDUP(preload, __PAGESIZE);
    r = EOK;
    adp = tmp->adp;
    if (flags & MAP_STACK) {
#ifdef STACK_GROWS_UP
        mm = ms->first;
        start = mm->start;
        end = start + preload - 1;
        if (end > mm->end)
            end = mm->end;
#else
        mm = ms->last;
        end = mm->end;
        start = (end - preload) + 1;
        if (start < mm->start)
            start = mm->start;
#endif
        todo = (end - start) + 1;
        if (mm->extra_flags & EXTRA_FLAG_PRIMARY_STK) {
            if (rlimit_blown(pid, RLIMIT_STACK, todo)) {
                return ENOMEM;
            }
            tmp->rlimit.stack = todo;
        }
        r = memory_reference(
            &mm, start, end,
            (mm->mmap_flags & PROT_WRITE) ? (MR_WRITE | MR_TRUNC) : MR_TRUNC,
            ms);
    } else {
        /* memory_reference() might be fiddling the list */
        uintptr_t end_vaddr = ms->last->end;

        mr_flags = (flags & PROT_WRITE) ? (MR_WRITE | MR_TRUNC) : MR_TRUNC;
        mm = ms->first;
        for (;;) {
            or = mm->obj_ref;
            if (or) {
                obp = or->obp;
                start = mm->start;
                end = mm->end;
                maxsize = (end - start) + 1;
                todo = preload;
                if (todo > maxsize)
                    todo = maxsize;
                if (!(flags & MAP_SYSRAM)) {
                    /* Doing a direct physical mapping - just set up the page
                     * tables right away.
                     */
                    r = memory_reference(&mm, start, end, MR_NOINIT, ms);
                } else {
                    uintptr_t s;
                    uintptr_t e;

                    if (todo > 0) {
                        r = memory_reference(&mm, start, start + (todo - 1),
                                             mr_flags, ms);
                        if (r != EOK)
                            return r;
                    }
                    /* Careful - memory_reference() might adjust 'mm' */
                    s = start + todo;
                    e = mm->end;

                    if (s < e) {
                        /* Have to make sure all the L2's are allocated */
                        r = pte_prealloc(adp, s, e);
                        if ((r == EOK) && (mm->obj_ref->obp == obp)) {
                            /* If we happen to have init'd pages, set up the
                             * PTE's.
                             */
                            r = memory_reference(&mm, s, e, MR_NOINIT, ms);
                        }
                    }
                }
                if (r != EOK)
                    return r;
                preload -= todo;
            }
            if (mm->end >= end_vaddr)
                break;
            mm = mm->next;
        }
    }
    return r;
}


/* Create a memory mapping in a process (or the system) address space.
 *
 * The as implementation of mmap. Handles system-memory mappings (!prp:
 * anonymous, physical, and loader code/data hints) and user mappings
 * (anonymous, file-backed, physical, shared, and typed memory), allocating
 * virtual address space and physical backing, enforcing rlimits, then faulting
 * in the requested pages. Backs out cleanly through the fail* labels on any
 * error. Parameters:
 *   pid             : target process pid (0 for system mappings)
 *   prp             : target process (kernel-side handle; NULL for system)
 *   vaddr_requested : requested virtual address (0 = let the mapper choose)
 *   size_requested  : requested size in bytes
 *   prot            : PROT_* protection bits
 *   flags           : MAP_* / IMAP_* flags
 *   obp             : backing memory object (NULL for anon/phys)
 *   boff            : object offset (or physical address for MAP_PHYS)
 *   alignval        : requested alignment (currently advisory)
 *   preload         : bytes to fault in up front
 *   fd              : backing fd, or NOFD
 *   vaddrp          : OUT — receives the mapped virtual address
 *   sizep           : OUT — receives the mapped size
 * Return value: EOK on success, or an errno on failure.
 */
int as_mmap(pid_t pid,
            process_t *prp,
            uintptr_t vaddr_requested,
            size_t size_requested,
            int prot,
            int flags,
            path_object_t *obp,
            uint64_t boff,
            size_t alignval,
            size_t preload,
            int fd,
            void **vaddrp,
            size_t *sizep)
{
    uintptr_t vaddr;
    size_t size;
    size_t guardsize = 0;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;

    /* Snapshot kernel-side prp fields once: U-mode roottask cannot deref the
     * kernel-allocated process_t directly (heap is PTE_U=0).
     */
    root_proc_info_t prp_info;
    int prp_info_valid = 0;
    if (pid != 0 && process_query_info(pid, &prp_info) == EOK)
        prp_info_valid = 1;
    root_trace(
        "as_mmap ENTER: pid=%d va=%p sz=%lx prot=%x flags=%x fd=%d obp=%p "
        "boff=%lx\n",
        pid, (void *) vaddr_requested, (unsigned long) size_requested, prot,
        flags, fd, (void *) obp, (unsigned long) boff);
    size_t pg_offset;
    address_t *adp;
    struct mm_map_head *mh;
    struct map_set ms;
    struct map_set ms_repl;
    int r;
    struct mm_map *mm;
    struct pa_quantum *pq = NULL;
    int pq_linked = 0;
    uintptr_t mask;
    unsigned extra_flags = 0;
    unsigned create_flags;
    path_object_t *anmem_obp;
    off64_t anmem_off;
    void *tail_vaddr;
    size_t tail_size;

    /* RUSH2: /proc/<pid>/as support. Remember to turn on the
     * MM_ANMEM_MULTI_REFS RUSH2: bit for anonymous objects so we don't skip
     * check_last_ref RUSH2: in as_munmap. RUSH2: What should I do with
     * 'alignval'?
     */

    flags |= prot;
    if (flags & MAP_STACK) {
        if (!(flags & MAP_ANON) || ((size_t) boff >= size_requested)) {
            return EINVAL;
        }
        /* calculate guardpage size (in boff) */
        guardsize = ROUNDUP((size_t) boff, __PAGESIZE);

        /* boff < size_requested was checked, but the page round-up can push
         * guardsize up to (or past) size_requested — a guard region that
         * consumes the whole mapping leaves no stack and drives map_split()
         * into its no-split crash() path, a deterministic roottask DoS from an
         * untrusted _MEM_MAP. Require at least one page of usable stack.
         */
        if (guardsize >= size_requested) {
            return EINVAL;
        }
        boff = 0;
    }

    if (!prp) {
        /* mmap system memory... */

        /* MAP_SYSRAM records the roottask image ranges while the loader maps
         * the boot image.
         */
        if (flags & MAP_SYSRAM) {
            struct vaddr_range *seg;

            if (prot & PROT_WRITE) {
                seg = &system_code_vaddr;
            } else {
                seg = &system_data_vaddr;
            }
            seg->lo = vaddr_requested;
            seg->hi = vaddr_requested + size_requested - 1;
            return EOK;
        }

        if ((obp) && (obp->hdr.type == OBJECT_MEM_SHARED) &&
            (obp->mem.mm.flags & SHMCTL_PHYS) &&
            !(obp->mem.mm.flags & SHMCTL_ANON)) {
            /* Direct mapping */
            boff += pa_quantum_to_paddr(obp->mem.mm.pmem);
            flags &= ~MAP_ANON;
            flags |= MAP_PHYS;
            obp = NULL;
        }

        CRASHCHECK(flags & PROT_NOCACHE);
        CRASHCHECK(obp);

        if (flags & MAP_STACK)
            size_requested -= guardsize;
        if (flags & MAP_ANON) {
            /* We're always asking for contiguous memory, but that might not
             * always be required. It should be OK though, since the vast
             * majority of system allocations are only going to be one page.
             */
            size_requested = ROUNDUP(size_requested, QUANTUM_SIZE);

            /* PAQ_FLAG_SYSTEM has to be set on the freshly-allocated pq. pq
             * lives in the kernel heap (PTE_U=0); U-mode roottask cannot OR
             * into pq->flags directly, so we ask the pa_alloc kernel extension
             * to do it for us atomically.
             */
            extern struct pa_quantum *pa_alloc_with_qflags(
                paddr_t, paddr_t, unsigned, unsigned, unsigned *,
                struct pa_restrict *, paddr_t, unsigned);
            pq = pa_alloc_with_qflags(size_requested, 0, PAQ_COLOUR_NONE,
                                      PAA_FLAG_CONTIG, NULL, restrict_proc, 0,
                                      PAQ_FLAG_SYSTEM);
            if (!pq) {
                return ENOMEM;
            }
            boff = pa_quantum_to_paddr(pq);
            pg_offset = 0;
        } else if (flags & MAP_PHYS) {
            /* Have to validate the paddr (passed in in 'boff') because mounting
             * an image file system gets the paddr from user input and bypasses
             * the normal address-space map validation path.
             */
            if ((boff > last_paddr) ||
                ((boff + size_requested - 1)) > last_paddr) {
                return EINVAL;
            }
            pg_offset = boff - ROUNDDOWN(boff, __PAGESIZE);
            boff -= pg_offset;
            size_requested = ROUNDUP(size_requested + pg_offset, QUANTUM_SIZE);
        } else {
            crash();
            /* NOTREACHED */
            pg_offset = 0;
        }
        *sizep = size_requested;
        if (CPU_1TO1_IS_PADDR(boff)) {
            vaddr = (uintptr_t) boff + CPU_1TO1_VADDR_BIAS;
        } else {
#if CPU_SYSTEM_PADDR_MUST
            if (!(flags & MAP_ANON)) {
                /* image file systems mount - can't do it in the sysaddr space
                 * if we're outside the 1-to-1 region.
                 */
                return ENOTSUP;
            }
            crash();
            /* NOTREACHED */
            vaddr = 0;
#else
            struct mm_pte_manipulate data;

            data.end = size_requested;
            data.prot = (unsigned) flags;
            data.paddr = (paddr_t) boff;
            syscall_priv(__KSYS_PRIV_SYSADDR_MAP, &data);
            vaddr = data.start;
            if (vaddr == VA_INVALID) {
                if (flags & MAP_ANON) {
                    pa_free_list(pq, 0);
                }
                return ENOMEM;
            }
#endif
        }
        *vaddrp = (uint8_t *) vaddr + pg_offset;
        if (pq) {
            if (vaddr < system_heap_vaddr.lo)
                system_heap_vaddr.lo = vaddr;
            vaddr += size_requested - 1;
            if (vaddr > system_heap_vaddr.hi)
                system_heap_vaddr.hi = vaddr;
        }
        return EOK;
    }

    /* mmap user memory... */
    adp = tmp ? tmp->adp : NULL;

    /* make sure our heap hasn't exceeded its rlimit */
    if ((flags & (MAP_ANON | MAP_STACK | MAP_ELF | MAP_TYPE)) ==
        (MAP_ANON | MAP_PRIVATE)) {
        if (rlimit_blown(pid, RLIMIT_DATA, tmp->rlimit.data + size_requested)) {
            return ENOMEM;
        }
        extra_flags |= EXTRA_FLAG_RLIMIT_DATA;
    }

    /* make sure our total vm size hasn't exceeded its rlimit */
    if (rlimit_blown(pid, RLIMIT_AS, tmp->rlimit.vmem + size_requested)) {
        return ENOMEM;
    }

    tail_vaddr = NULL;
    tail_size = 0;
    if ((obp) &&
        ((flags & (MAP_TYPE | MAP_ANON | MAP_ELF | MAP_FIXED)) ==
         (MAP_PRIVATE | MAP_ELF | MAP_FIXED)) &&
        !(prp_info_valid && (prp_info.flags & RTMUX_FLG_PROC_LOADING))) {
        uintptr_t end_vaddr;

        switch (obp->hdr.type) {
        case OBJECT_MEM_FD:
        case OBJECT_MEM_SHARED:
            end_vaddr = ROUNDUP(vaddr_requested + size_requested, __PAGESIZE);
            vaddr = ROUNDUP(
                vaddr_requested + (size_t) (obp->mem.mm.size - (off64_t) boff),
                __PAGESIZE);
            if (vaddr < end_vaddr) {
                r = as_mmap(pid, prp, vaddr, end_vaddr - vaddr,
                            prot & PROT_MASK,
                            MAP_PRIVATE | MAP_ANON | MAP_FIXED, NULL, 0, 0, 0,
                            NOFD, &tail_vaddr, &tail_size);
                if (r != EOK)
                    return r;
                size_requested = vaddr - vaddr_requested;
            }
            break;
        default:
            break;
        }
    }

    if (prp_info_valid && (prp_info.flags & RTMUX_FLG_PROC_LOADING))
        extra_flags |= EXTRA_FLAG_LOADER;
    if (flags & IMAP_OCB_RDONLY) {
        if ((flags & PROT_WRITE) && !(extra_flags & EXTRA_FLAG_LOADER)) {
            /* Report an error if we try to gain PROT_WRITE perms to an object
             * that's opened for read-only, unless it's the loader doing it -
             * that happens for use-in-place data segments
             */
            r = EACCES;
            goto fail0;
        }
        extra_flags |= EXTRA_FLAG_RDONLY;
    }

    pg_offset = ADDR_OFFSET((size_t) boff);
    if (flags & MAP_FIXED) {
        unsigned addr_offset;

        r = EINVAL; /* Assuming a failure in this section */
        /* Make sure addr and offset are at the same offset modulo pagesize */
        addr_offset = ADDR_OFFSET(vaddr_requested);
        if (pg_offset != addr_offset) {
            pg_offset = addr_offset;
            if (!obp) {
                /* physical mappings have to have addr/offset congruency */
                if ((flags & MAP_PHYS) && !(flags & MAP_ANON))
                    goto fail1;
            } else if (obp->hdr.type == OBJECT_MEM_TYPED) {
                /* non-allocated typed memory have to have addr/offset
                 * congruency
                 */
                if (!(flags &
                      (IMAP_TYMEM_ALLOCATE | IMAP_TYMEM_ALLOCATE_CONTIG)))
                    goto fail1;
            } else {
                /* otherwise addr & offset have to be congruent */
                goto fail1;
            }
        }
        /* Don't allow a guardsize with MAP_FIXED */
        guardsize = 0;
        vaddr = vaddr_requested;
    } else {
        if (flags & MAP_STACK) {
            vaddr = (prp_info_valid ? prp_info.base_addr : 0);
        } else if (vaddr_requested != 0) {
            vaddr = vaddr_requested;
            if (!(flags & MAP_BELOW)) {
                vaddr = ROUNDUP(vaddr, __PAGESIZE);
            }
        } else if (flags & MAP_ELF) {
            vaddr = CPU_SO_VADDR_START;
        } else if (flags & MAP_ANON) {
            vaddr = (prp_info_valid ? prp_info.base_addr : 0);
        } else {
            vaddr = CPU_SHMEM_VADDR_START;
        }
    }
    boff -= pg_offset;
    size = ROUNDUP(size_requested + pg_offset, __PAGESIZE);
    if (size < size_requested) {
        r = ENOMEM;
        goto fail1;
    }

    mh = &tmp->map;

    mask = 0;

    anmem_obp = tmp->anon;
    if (!anmem_obp) {
        /* No anonymous-memory object: the address space is being torn down
         * (as_mdestroy_pid). Every path below uses it, so refuse here.
         */
        r = ENOMEM;
        goto fail1;
    }

    /* RUSH2: Need to deal with alignment RUSH2: Need to deal with
     * /proc/<pid>/as objects...
     */
    create_flags = (unsigned) flags;
#ifndef STACK_GROWS_UP
    if (create_flags & MAP_STACK)
        create_flags |= MAP_BELOW;
#endif
    if ((mask == 0) && (vaddr_requested == 0) && (mm_flags & MM_FLAG_VPS)) {
        /* If we don't have an imposed alignment and no restrictions on the
         * vaddr we allocate, tell map_create() that it should try to make (but
         * not require) the allocated vaddr to be aligned in such a manner that
         * we can use big pages, but first check that the mapping could possibly
         * create a big page.
         */
        switch (CPU_SYSTEM_HAVE_MULTIPLE_PAGESIZES) {
        case VPS_HIGHUSAGE:
            /* Only shared memory objects with SHMCTL_HIGHUSAGE can potentially
             * use large pages.
             */
            if ((!obp) || (obp->hdr.type != OBJECT_MEM_SHARED) ||
                !(obp->mem.mm.flags & SHMCTL_HIGHUSAGE))
                break;
            /* Fall through */
        case VPS_AUTO:
            /* All mappings could potentially use big pages. */
            mask = 1;
            break;
        default:
            /* VPS_NONE: No mappings will ever use a big page. */
            break;
        }
    }
    r = map_create(&ms, &ms_repl, mh, ADDR_PAGE(vaddr), size, mask,
                   create_flags);
    if (r != EOK)
        goto fail1;
    mm = ms.first;
    if ((flags & MAP_STACK) && (extra_flags & EXTRA_FLAG_LOADER)) {
        /* If it's a MAP_STACK while we're loading the process, this must be the
         * primary stack - remember that for RLIMIT_STACK handling.
         */
        extra_flags |= EXTRA_FLAG_PRIMARY_STK;
    }

    if ((flags & (MAP_TYPE | MAP_ELF | MAP_FIXED)) ==
        (MAP_ELF | MAP_FIXED | MAP_PRIVATE)) {
        uintptr_t end;

        /* For MAP_ELF, the size_requested is the phdr->p_filesz field, so we
         * really can't read beyond that point even if the file object allows it
         * - the data may not be all zeros. Instead we remember how much of the
         * last page is 'missing' and code in mm-reference.c will zero it when
         * the page gets initialized.
         */

        /* RUSH3: This is assuming that only the MAP_PRIVATE mapping RUSH3: has
         * phdr->p_filesz < phdr->p_memsz and that there's RUSH3: no other
         * segment data following it. Think about reading RUSH3: in the segment
         * table in vm-fd.c so that we can consult RUSH3: it in mm-reference.c
         * (and we wouldn't need this code anymore, RUSH3: just a bit in
         * extra_flags that pq_init() needed to do some RUSH3: extra checks).
         */
        end = vaddr_requested + size_requested;
        mm->last_page_bss = (unsigned short) (ROUNDUP(end, __PAGESIZE) - end);
    }

    vaddr = mm->start;
    if (guardsize != 0) {
        r = map_split(&ms, guardsize);
        if (r != EOK)
            goto fail2;
        size -= guardsize;
        mm = ms.first;
#ifdef STACK_GROWS_UP
        /* With an upwardly growing stack, the guard page is at end of list */
        mm->next->mmap_flags = MAP_STACK;
#else
        mm->mmap_flags = MAP_STACK;
        mm = mm->next;
#endif
    }

    /* RUSH3: We can get more sophisticated and only zero the temp mapping
     * RUSH3: area size if the [vaddr, mm->end) range is overlapping with RUSH3:
     * it.
     */
    tmp->tmap_size = 0;

    anmem_off = ~(off64_t) 0;
    if (!obp) {
        obp = anmem_obp;
        if (ANMEM_MULTI_REFS(obp)) {
            memobj_lock(obp);
        }
    }

    if (!(flags & MAP_ANON)) {
        r = memobj_offset_check(obp, (off64_t) boff, (uint64_t) size);
        if (r != EOK)
            goto fail2;
    }

    if (ms_repl.first) {
        r = ms_unmap(pid, adp, &ms_repl, 0);
        if (r != EOK)
            goto fail2;
    }

    if (obp->hdr.type == OBJECT_MEM_TYPED) {
        memobj_lock(obp);
        r = tymem_pmem_alloc(prp, obp, &ms, mm, (unsigned) flags,
                             (off64_t) boff, size);
        memobj_unlock(obp);
        if (r != EOK)
            goto fail3;
        if (flags & (IMAP_TYMEM_ALLOCATE | IMAP_TYMEM_ALLOCATE_CONTIG)) {
            if (!(obp->mem.mm.flags & MM_MEM_RDB)) {
                flags |= MAP_SYSRAM;
            }
        }
    } else if (obp->hdr.type == OBJECT_MEM_FD) {
        r = memobj_pmem_add(obp, (off64_t) boff, size, flags);
        if (r != EOK)
            goto fail3;
        flags |= MAP_SYSRAM;
    } else if (flags & MAP_ANON) {
        boff = (uint64_t) anmem_offset(obp, mm->start, size);
        r = memobj_pmem_add(obp, (off64_t) boff, size, flags);
        if (r != EOK)
            goto fail3;
        flags |= MAP_SYSRAM;
    } else if (flags & MAP_PHYS) {
        off64_t noff;

        noff = anmem_offset(obp, mm->start, size);
        pq = pa_alloc_fake((paddr_t) ADDR_PAGE(boff), (paddr_t) size);
        if (!pq)
            goto fail3;
        r = memobj_pmem_add_pq(obp, noff, pq);
        if (r != EOK)
            goto fail4;
        pq_linked = 1;
        boff = (uint64_t) noff;
    } else if (obp->hdr.type == OBJECT_MEM_SHARED) {
        if ((obp->mem.mm.flags &
             (MM_SHMEM_SPECIAL | SHMCTL_ANON | SHMCTL_PHYS)) !=
            (MM_SHMEM_SPECIAL | SHMCTL_PHYS)) {
            flags |= MAP_SYSRAM;
        }
    }
    if (((flags & MAP_TYPE) == MAP_PRIVATE) &&
        (obp->hdr.type != OBJECT_MEM_ANON)) {
        /* For MAP_PRIVATE, non anonymous memory, pre-allocate all anonymous
         * memory that we might end up needing for copy-on-write. This avoids
         * overcommitting memory and having processes randomly dying with
         * SIGBUS's.
         */
        memobj_lock(anmem_obp);
        anmem_off = anmem_offset(anmem_obp, mm->start, size);
        r = memobj_pmem_add(anmem_obp, anmem_off, size, flags);
        memobj_unlock(anmem_obp);
        if (r != EOK)
            goto fail4;
    }

    r = map_add(&ms);
    if (r != EOK)
        goto fail5;
    for (;;) {
        /* Typed memory offsets already set by tymem_pmem_alloc()... */
        if (obp->hdr.type != OBJECT_MEM_TYPED) {
            mm->offset = (off64_t) boff;
        }

        /* The IMAP_MASK includes MAP_SYSRAM, which we want to allow through
         * here
         */
        mm->mmap_flags =
            (unsigned) flags & (~(unsigned) IMAP_MASK | MAP_SYSRAM);
        mm->extra_flags |= extra_flags;
        r = memref_add(mm, obp, pid, adp, fd);
        if (r != EOK)
            goto fail6;
        if (mm == ms.last)
            break;
        boff += (mm->end - mm->start) + 1;
        mm = mm->next;
    }

    /* No demand paging: all pages are allocated upfront. */
    preload = size;
    r = setup_mappings(pid, prp, &ms, (unsigned) flags, preload);

    /* RUSH2: What if obj == anmem_obp on entry (think "/proc/self/as") */
    if ((obp == anmem_obp) && ANMEM_MULTI_REFS(obp)) {
        memobj_unlock(obp);
    }

    if (r != EOK)
        goto fail7;

    size += guardsize;
    if (extra_flags & EXTRA_FLAG_RLIMIT_DATA) {
        tmp->rlimit.data += size;
    }
    tmp->rlimit.vmem += size;

    /* Make sure we tell the root_vm_ops we're done with the mm. This will also
     * coalesce this mapping with any adjacent ones if possible.
     */
    map_coalese(&ms);

    *vaddrp = (void *) (vaddr + pg_offset);
    size += tail_size;
    *sizep = size - pg_offset;

    return EOK;

fail7:
    (void) ms_unmap(pid, adp, &ms, UNMAP_NORLIMIT);
    return r;

fail6: {
    struct mm_map *del;

    for (del = ms.first; del != mm; del = del->next) {
        if (del->obj_ref) {
            memref_del(del);
        }
    }
}
    map_remove(&ms);

fail5:
    if (anmem_off != ~(off64_t) 0) {
        memobj_lock(anmem_obp);
        memobj_pmem_del_len(anmem_obp, anmem_off, size);
        memobj_unlock(anmem_obp);
    }

fail4:
    if (obp->hdr.type == OBJECT_MEM_TYPED) {
        /* Free the memory allocated in tymem_pmem_alloc() */
        memobj_lock(obp);
        for (mm = ms.first; mm != ms.last->next; mm = mm->next) {
            memobj_pmem_del_len(obp, mm->offset, (mm->end - mm->start) + 1);
        }
        memobj_unlock(obp);
    } else if (flags & MAP_ANON) {
        memobj_pmem_del_len(obp, (off64_t) boff, size);
    } else if (pq_linked) {
        memobj_pmem_del_len(obp, (off64_t) boff, size);
    }
    if (!pq_linked && (pq) && (pa_pq_blk(pq) == PAQ_BLK_FAKE)) {
        /* This is actually a struct pa_quantum_fake */
        pa_free_fake(pq);
    }

fail3:
    if (flags & (MAP_ANON | MAP_PHYS)) {
        anmem_unoffset(obp, (off64_t) boff, size);
    }

fail2:
    map_destroy(&ms);
    if (ms_repl.first)
        map_coalese(&ms_repl);

    if ((obp == anmem_obp) && ANMEM_MULTI_REFS(obp)) {
        memobj_unlock(obp);
    }

fail1:
fail0:
    if (tail_vaddr) {
        (void) as_munmap(pid, prp, (uintptr_t) tail_vaddr, tail_size, 0);
    }
    return r;
}
