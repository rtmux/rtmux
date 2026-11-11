/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/procfs.h>
#include "as.h"
#include "pa-pq.h"
#include "root-process.h"

struct vaddr_range system_code_vaddr;
struct vaddr_range system_data_vaddr;

struct pmem_data {
    off64_t off;
    paddr_t paddr_next;
    unsigned flags;
    size_t len;
    int need_contig;
};


static int pmem_info(path_object_t *obp,
                     off64_t off,
                     struct pa_quantum *pq,
                     size_t num,
                     void *d)
{
    struct pmem_data *data = d;
    paddr_t paddr;
    size_t len;
    unsigned flags;

    len = NQUANTUM_TO_LEN(num);

    if (!pq) {
        if (data->len == 0) {
            data->len = len;
        }
        return -1;
    }

    paddr = pa_quantum_to_paddr(pq);
    if ((data->len != 0) && data->need_contig && (paddr != data->paddr_next)) {
        return -1;
    }
    if (pa_pq_blk(pq) == PAQ_BLK_FAKE) {
        flags = MM_PG_HWMAPPED | MM_PG_REFERENCED | MM_PG_MODIFIED;
        if (data->len == 0) {
            unsigned skip = (unsigned) (data->off - off);

            /* We only have a single pa_quantum_fake to cover the whole of the
             * run, so we need to adjust the paddr with the offset from the
             * start.
             */
            len -= skip;
            paddr += skip;
        }
    } else {
        flags = MM_PG_HWMAPPED | MM_PG_REFERENCED | MM_PG_MODIFIED;
    }
    if (data->len == 0) {
        data->flags = flags;
    }
    if (flags != data->flags) {
        return -1;
    }
    data->len += len;
    data->paddr_next = paddr + len;
    return EOK;
}


/* Entry: prp locked process pointer to lookup mapping on vaddr virtual address
 * to lookup within process mip map info pointer (If non-zero vaddr matching
 * will match next higher entry. i.e. next valid vaddr for process) mdp map
 * debug pointer (stuffed with debugger vaddr and debugger file name) obpp
 * stuffed with path_object_t pointer for the region fdp stuffed with file
 * descriptor for the region contigp stuffed with max contiguous length of the
 * region
 * Returns: number of valid bytes from vaddr (or start of returned mapping if
 * vaddr is not within any mappings) to end of mapping
 */

size_t as_mapinfo(pid_t pid,
                  process_t *prp,
                  uintptr_t vaddr,
                  struct _procfs_map_info *mip,
                  struct _procfs_debug_info *mdp,
                  size_t dbginfo_pathlen,
                  path_object_t **obpp,
                  int *fdp,
                  size_t *contigp)
{
    address_t *adp;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;
    root_proc_info_t prp_info;
    int prp_info_valid =
        (pid != 0 && process_query_info(pid, &prp_info) == EOK);
    struct _procfs_map_info info;
    struct map_set ms;
    struct mm_map *mm;
    struct mm_map *name_mm;
    struct mm_object_ref * or ;
    path_object_t *obp;
    struct pmem_data data;
    uintptr_t chk_vaddr;

    if (obpp)
        *obpp = NULL;
    if (fdp)
        *fdp = NOFD;
    adp = tmp ? tmp->adp : NULL;
    if (!adp)
        return 0;

    memset(&info, 0, sizeof(info));

    if (pid == ROOTTASK_PID) {
        if (prp_info_valid && vaddr == prp_info.base_addr) {
            vaddr = 0;
        }
        info.dev = 1;
        info.ino = PID_TO_INO(ROOTTASK_PID, 0);

        /* RUSH1: need special handling to get info on system address space.
         * RUSH1: Information contained in system_heap_vaddr, system_code_vaddr
         * RUSH1: and system_data_vaddr variables - for the heap, additional
         * RUSH1: inspection of the physical allocator structures is required
         * Things to fill in: info (*mip): uint64_t vaddr; uint64_t size;
         * uint32_t flags; dev_t dev; off_t offset; ino_t ino;
         *
         * if(mdp) { mdp->vaddr = vaddr; if(mem from heap) { mdp->path[0] =
         * '\0'; } else { STRLCPY(mdp->path, "kernel", dbginfo_pathlen); } }
         */
        return 0;
    }
    chk_vaddr = vaddr;
    for (;;) {
        map_isolate(&ms, &tmp->map, chk_vaddr, 0, MI_NEXT);
        mm = ms.first;
        if (!mm)
            return 0;
        or = mm->obj_ref;
        if (or)
            break;
        if (mm->mmap_flags & MAP_STACK)
            break; /* Need to report guard pages */
        map_coalese(&ms);
        chk_vaddr = mm->end + 1;
    }
    if (vaddr < mm->start) {
        vaddr = mm->start;
    }
    info.vaddr = vaddr;
    info.size = mm->end + 1 - vaddr;
    info.flags = mm->mmap_flags;
    if (or) {
        obp = or->obp;
        memobj_lock(obp);
        if (obpp)
            *obpp = obp;
        if (fdp)
            *fdp = or->fd;
        name_mm = mm;
        if ((mm->mmap_flags & MAP_ELF) && (mm->mmap_flags & PROT_WRITE)) {
            struct map_set pms;
            struct mm_map *prev_mm;

            /* For MAP_ELF objects, set ino and name to the text segment base so
             * /proc map readers can match text/data for a shared library. RUSH
             * - as pointed below, we really need to keep a reference to the
             * object/shared lib for the data segment. This code is just
             * heuristics and not guaranteed to always work.
             */

            /* This is the data segment - text should be before */
            prev_mm = mm;
            for (;;) {
                map_isolate(&pms, &tmp->map, prev_mm->start - 1, 0, MI_NEXT);
                if (pms.first == prev_mm)
                    break;
                prev_mm = pms.first;
                if (!prev_mm)
                    break;
                if (!prev_mm->obj_ref)
                    break;
                if (!(prev_mm->mmap_flags & MAP_ELF))
                    break;
                if ((prev_mm->mmap_flags & (PROT_WRITE | PROT_EXEC)) ==
                    PROT_EXEC) {
                    name_mm = prev_mm;
                    break;
                }
                map_coalese(&pms);
            }
        }

        if (mdp) {
            path_object_t *nobp = name_mm->obj_ref->obp;

            /* Writable private data from a shared object is reported like a
             * normal anonymous mapping after copy-on-write. Preserving the
             * original shared-object name would require another back-reference
             * in mm_map, and no POSIX or LKL caller relies on it.
             */
            mdp->vaddr = vaddr - (uintptr_t) mm->reloc;
            if (object_name(nobp, dbginfo_pathlen, mdp->path) == 0) {
                /* Loader-mapped exec name lives at root_proc(pid)->debug_name
                 * (roottask side) post heap-PTE_U=0. Stamp it into the mapdebug
                 * record when the segment is the loaded image itself; otherwise
                 * fall through to the generic /dev/zero or /dev/mem name.
                 */
                root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;
                if ((name_mm->extra_flags & EXTRA_FLAG_LOADER) &&
                    (name_mm->mmap_flags & MAP_ELF) && tmp && tmp->debug_name) {
                    STRLCPY(mdp->path, tmp->debug_name, dbginfo_pathlen);
                } else if ((nobp->hdr.type == OBJECT_MEM_ANON) &&
                           (mm->mmap_flags & MAP_SYSRAM)) {
                    STRLCPY(mdp->path, "/dev/zero", dbginfo_pathlen);

                    /* RUSH2: Do something with OBJECT_MEM_TYPED here, or in
                     * tymem_name?
                     */
                } else {
                    STRLCPY(mdp->path, "/dev/mem", dbginfo_pathlen);
                }
            }
        }
        info.offset = (off_t) ((uint64_t) mm->offset + (vaddr - mm->start));
        if ((contigp) || (mip)) {
            data.flags = 0;
            data.len = 0;
            data.off = info.offset;
            data.need_contig = !!contigp;
            memobj_pmem_walk(MPW_SYSRAM | MPW_PHYS | MPW_HOLES, obp,
                             info.offset,
                             (off64_t) ((size_t) info.offset + info.size - 1),
                             pmem_info, &data);

            info.size = data.len - ADDR_OFFSET(vaddr);
            if (contigp) {
                *contigp = data.len;
            }
            if (mip) {
                if (mm->mmap_flags & MAP_ELF) {
                    info.dev = 4;
                    info.ino = name_mm->start;
                    info.offset = (off_t) ((uint64_t) name_mm->offset +
                                           (vaddr - name_mm->start));
                } else if (obp->hdr.type == OBJECT_MEM_SHARED) {
                    info.dev = 3;
                    info.ino = 1;
                    /* RUSH2: Do something with OBJECT_MEM_TYPED here? */
                } else {
                    info.dev = 2;
                    if ((mm->mmap_flags & MAP_STACK) && (data.flags == 0)) {
                        /* Override ino so /proc map readers display the lazy
                         * part correctly.
                         */
                        info.ino = 0;
                    } else {
                        info.ino = 1;
                    }

                    /* Anonymous mappings don't have an offset */
                    if (obp->hdr.type == OBJECT_MEM_ANON)
                        info.offset = 0;
                }
                info.flags |= data.flags;
            }
        }
        memobj_unlock(obp);
    } else if (mdp) {
        /* This is a stack guard page. */
        mdp->vaddr = vaddr;
        mdp->path[0] = '\0';
    }
    map_coalese(&ms);

    if (mip)
        *mip = info;

    return info.size;
}
