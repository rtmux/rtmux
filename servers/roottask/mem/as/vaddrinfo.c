/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"
#include "pa-pq.h"
#include "root-process.h"

/* ext_vaddrinfo body relocated to kernel/ext/vaddrinfo.c as part of
 * U-mode-roottask migration.
 */
extern void ext_vaddrinfo(void *data);

struct args_vaddrinfo {
    off64_t off;
    paddr_t paddr;
    process_t *prp;
    uintptr_t vaddr;
    struct mm_map *mm;
    int init;
    int found;
    unsigned pg_offset;
    size_t len;
};

static int get_paddr(path_object_t *obp,
                     off64_t off,
                     struct pa_quantum *pq,
                     size_t num,
                     void *d)
{
    struct args_vaddrinfo *data = d;
    uintptr_t vaddr;
    struct mm_map *mm;
    size_t skip;

    data->paddr = pa_quantum_to_paddr(pq) + data->pg_offset;
    if (pa_pq_blk(pq) == PAQ_BLK_FAKE) {
        /* We only have a single pa_quantum_fake to cover the whole of the run,
         * so we need to adjust the paddr with the offset from the start.
         */
        skip = (size_t) (data->off - off);
        data->paddr += skip;
        data->len = NQUANTUM_TO_LEN(pa_pq_run(pq)) - skip - data->pg_offset;
    } else {
        data->len = QUANTUM_SIZE - data->pg_offset;
        mm = data->mm;
        vaddr = mm->start + (size_t) (off - mm->offset);
        data->paddr += ADDR_OFFSET(vaddr);
        if (data->init) {
            /* Scan through the physically contiguous run of memory. If the page
             * hasn't been initialized yet, do it now. A driver might be
             * programing some hardware to DMA into the memory and we don't want
             * to overwrite that data later...
             */
            for (;;) {
                int run;
                if (!(pa_pq_flags(pq) & PAQ_FLAG_INITIALIZED)) {
                    (void) memory_reference(&data->mm, vaddr,
                                            vaddr | (QUANTUM_SIZE - 1), MR_NONE,
                                            NULL);
                }
                run = pa_pq_run(pq);
                if (run < 0)
                    break;
                if (run == 1)
                    break;
                vaddr += QUANTUM_SIZE;
                if (vaddr > mm->end)
                    break;
                data->len += QUANTUM_SIZE;
                ++pq;
            }
        }
    }
    data->found = 1;
    return -1;
}

/* FUTURE: Do we need to go into the kernel for this? Have to mux with FUTURE:
 * page table destruction if not.
 */

unsigned as_vaddrinfo(pid_t pid,
                      process_t *prp,
                      uintptr_t vaddr,
                      paddr_t *paddrp,
                      size_t *lenp,
                      unsigned flags)
{
    address_t *adp;
    root_process_t *tmp = (pid != 0) ? root_proc(pid) : NULL;
    struct map_set ms;
    struct mm_map *mm;
    path_object_t *obp;
    struct args_vaddrinfo data;
    int r;
    unsigned prot;
    int status;

    if (CPU_1TO1_IS_VADDR(vaddr)) {
        *paddrp = vaddr - CPU_1TO1_VADDR_BIAS;
        if (lenp)
            *lenp = ~(size_t) 0;
        return PROT_READ | PROT_WRITE | PROT_EXEC;
    }

    if (flags & VI_KDEBUG) {
        return cpu_as_vaddrinfo(prp, vaddr, paddrp, lenp);
    }

    prot = PROT_NONE;

    if ((tmp) && !(flags & VI_PGTBL) &&
        WITHIN_BOUNDARY(vaddr, vaddr, user_boundary_addr)) {
        data.init = flags & VI_INIT;
        data.found = 0;
        adp = tmp->adp;
        (void) adp;
        status = proc_rlock_adp(pid);
        CRASHCHECK(status == -1);
        r = map_isolate(&ms, &tmp->map, (uintptr_t) vaddr, 0, MI_NONE);
        if (r == EOK) {
            mm = ms.first;
            if (mm) {
                if (mm->obj_ref) {
                    data.mm = mm;
                    data.pg_offset = ADDR_OFFSET((uintptr_t) vaddr);
                    data.off =
                        (off64_t) ((uint64_t) mm->offset +
                                   (ADDR_PAGE((uintptr_t) vaddr) - mm->start));
                    obp = mm->obj_ref->obp;
                    memobj_lock(obp);
                    memobj_pmem_walk(MPW_PHYS | MPW_SYSRAM, obp, data.off,
                                     data.off, get_paddr, &data);
                    memobj_unlock(obp);

                    /* Turn on MAP_PHYS to make sure PROT_NONE isn't returned;
                     */
                    if (data.found) {
                        prot = mm->mmap_flags | MAP_PHYS;
                    }
                }
                map_coalese(&ms);
            }
        }
        proc_unlock_adp(pid);
    }

    if (prot == PROT_NONE) {
        /* If we didn't find anything in the upper level data structures, check
         * with the CPU specific code.
         */
        data.prp = prp;
        data.vaddr = vaddr;
        prot = (unsigned) syscall_priv(__KSYS_PRIV_VADDRINFO, &data);
    }

    *paddrp = data.paddr;
    if (lenp)
        *lenp = data.len;
    return prot;
}
