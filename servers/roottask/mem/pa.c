/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Roottask side of the physical-quantum allocator: thin U-mode wrappers over
 * the kernel-resident allocator (kernel/ext/pa.c). Each wrapper packs args into
 * a kcall_args_* struct and calls syscall_priv(__KSYS_PRIV_PA_*, &args).
 * Kernel-resident bodies acquire the BKL inside the syscall_priv dispatcher.
 *
 * pa_alloc_fake / pa_free_fake remain roottask-side: they create "fake"
 * pa_quanta (PAQ_BLK_FAKE) for memory regions that aren't in the sysram pool
 * (MMIO, foreign physical mappings). The struct lives in roottask heap
 * (_scalloc/_sfree); the kernel-side pa_quantum_to_paddr handles PAQ_BLK_FAKE
 * specially by reading the embedded paddr.
 */

/*
 * EXTERN-to-empty turns mm-internal.h's EXTERN-prefixed declarations
 * (restrict_proc, restrict_user, last_paddr, va_rover, mm_flags,
 * system_heap_vaddr, pgszlist) into definitions in this TU. pa.c is the sole
 * owner of these roottask-side globals and populates them from the OUT fields
 * of kext_pa_init.
 */
#define EXTERN

#include "as.h"
#include "externs.h"
#include "pa-priv.h"

/* blk_head is mirrored from kernel-side state, used only by a few roottask-side
 * scanners (vm_info etc.); kext_pa_init writes it.
 */
struct block_head **blk_head;
unsigned mm_flags = MM_FLAG_VPS;
int munmap_flags_default = MM_UNMAP_INIT_REQUIRED;

/* pa_alloc — physical allocation. Thin wrapper around the kernel's full
 * pa_alloc(), invoked via __KSYS_PRIV_PA_ALLOC_FULL. All pre-call shaping
 * (size==0 short-circuit, restriction walk, retry loop, purger invoke) lives
 * kernel-side now — those paths deref blk_head[] and the restriction chain,
 * both in kernel heap (PTE_U=0), so they cannot be run from U-mode roottask.
 */
struct pa_quantum *pa_alloc(paddr_t size,
                            paddr_t align,
                            unsigned color,
                            unsigned flags,
                            unsigned *status_p,
                            struct pa_restrict *restriction,
                            paddr_t resv_size)
{
    struct kcall_args_pa_alloc data;

    data.size = size;
    data.resv_size = resv_size;
    data.align = align;
    data.color = color;
    data.flags = flags;
    data.restriction = restriction;
    data.status = 0;
    data.alloc = NULL;
    data.state = 0;
    data.retries = 0;
    data.quantum_flags_or = 0;

    syscall_priv(__KSYS_PRIV_PA_ALLOC_FULL, &data);

    if (status_p)
        *status_p = data.status;
    return data.alloc;
}

/* pa_alloc_with_qflags — pa_alloc + atomic OR of quantum-state flags
 * (PAQ_FLAG_SYSTEM etc.). pq->flags is in kernel heap and not writable from
 * U-mode roottask; this kernel extension does the OR while the pq is still in
 * kernel context.
 */
struct pa_quantum *pa_alloc_with_qflags(paddr_t size,
                                        paddr_t align,
                                        unsigned color,
                                        unsigned flags,
                                        unsigned *status_p,
                                        struct pa_restrict *restriction,
                                        paddr_t resv_size,
                                        unsigned quantum_flags_or)
{
    struct kcall_args_pa_alloc data;

    data.size = size;
    data.resv_size = resv_size;
    data.align = align;
    data.color = color;
    data.flags = flags;
    data.restriction = restriction;
    data.status = 0;
    data.alloc = NULL;
    data.state = 0;
    data.retries = 0;
    data.quantum_flags_or = quantum_flags_or;

    syscall_priv(__KSYS_PRIV_PA_ALLOC_FULL, &data);

    if (status_p)
        *status_p = data.status;
    return data.alloc;
}


/* Allocate a specific run of already-located quanta.
 *
 * Parameters:
 *   pq       : first quantum of the run to claim
 *   num      : number of quanta
 *   status_p : OUT — receives the allocator status (may be NULL)
 * Return value: the kernel extension result code.
 */
int pa_alloc_given(struct pa_quantum *pq, size_t num, unsigned *status_p)
{
    struct kcall_args_pa_alloc_given data;
    data.pq = pq;
    data.num = num;
    data.status = status_p;
    return (int) syscall_priv(__KSYS_PRIV_PA_ALLOC_GIVEN, &data);
}


/* Free a run of quanta back to the physical allocator.
 *
 * Parameters:
 *   paq       : first quantum of the run to free
 *   num       : number of quanta (no-op if zero)
 *   resv_size : reserved-pool size adjustment to apply
 */
void _pa_free(struct pa_quantum *paq, size_t num, paddr_t resv_size)
{
    struct kcall_args_pa_free data;
    if (num == 0)
        return;
    data.paq = paq;
    data.num = num;
    data.resv_size = resv_size;
    syscall_priv(__KSYS_PRIV_PA_FREE, &data);
}


/* Free a run of quanta (public wrapper around _pa_free).
 *
 * Parameters:
 *   paq       : first quantum of the run to free
 *   num       : number of quanta
 *   resv_size : reserved-pool size adjustment to apply
 */
void pa_free(struct pa_quantum *paq, size_t num, paddr_t resv_size)
{
    _pa_free(paq, num, resv_size);
}


struct kcall_args_pa_free_paddr {
    paddr_t p;
    paddr_t s;
    paddr_t resv_size;
};

/* Free a physical range by address.
 *
 * Parameters:
 *   p         : physical start address
 *   s         : size in bytes
 *   resv_size : reserved-pool size adjustment to apply
 */
void pa_free_paddr(paddr_t p, paddr_t s, paddr_t resv_size)
{
    struct kcall_args_pa_free_paddr data;
    data.p = p;
    data.s = s;
    data.resv_size = resv_size;
    syscall_priv(__KSYS_PRIV_PA_FREE_PADDR, &data);
}


struct kcall_args_pa_free_list {
    struct pa_quantum *qp;
    paddr_t resv_size;
};

/* Free a linked list of quantum runs.
 *
 * Parameters:
 *   qp        : head of the pa_quantum list to free
 *   resv_size : reserved-pool size adjustment to apply
 */
void pa_free_list(struct pa_quantum *qp, paddr_t resv_size)
{
    struct kcall_args_pa_free_list data;
    data.qp = qp;
    data.resv_size = resv_size;
    syscall_priv(__KSYS_PRIV_PA_FREE_LIST, &data);
}


/* pa_alloc_fake / pa_free_fake — roottask-only. Allocate fake pa_quantum
 * structs (PAQ_BLK_FAKE) that wrap a paddr range without touching the sysram
 * pool. Used by mm_memobj for foreign-physical mappings and by vm_tymem for
 * typed-memory shims.
 */
struct pa_quantum *pa_alloc_fake(paddr_t paddr, paddr_t size)
{
    struct pa_quantum_fake *pq;

    pq = _scalloc(sizeof(*pq));
    if (!pq)
        return NULL;
    pq->paddr = paddr;
    pq->q.run = (int32_t) LEN_TO_NQUANTUM(size);
    pq->q.flags = PAQ_FLAG_INUSE | PAQ_COLOUR_NONE;
    pq->q.blk = PAQ_BLK_FAKE;
    return &pq->q;
}


void pa_free_fake(struct pa_quantum *pq)
{
    /* pq->blk is in roottask heap for fake quanta (the only valid input here),
     * so this read is safe. The kernel extension path would also work but adds
     * latency for no reason.
     */
    if (pq->blk != PAQ_BLK_FAKE)
        crash();
    _sfree(pq, sizeof(struct pa_quantum_fake));
}


/* pa_free_info — iterate the pa free-list and accumulate per- restriction
 * totals. The kernel extension does one freeq step per call; roottask applies
 * the user-supplied restriction filter between calls.
 */
void pa_free_info(struct pa_restrict *restriction,
                  paddr_t *totalp,
                  paddr_t *contigp)
{
    paddr_t total = 0, contig = 0, start, end;
    struct block_head **bhp;
    struct pa_restrict *rp;
    struct kcall_args_pa_free_info data;

    bhp = blk_head;
    for (;;) {
        data.bh = *bhp++;
        if (!data.bh)
            break;
        data.generation = data.bh->generation - 1; /* force restart */
        while (syscall_priv(__KSYS_PRIV_PA_FREE_INFO, &data)) {
            for (rp = restriction; rp; rp = rp->next) {
                start = max(data.run_start, rp->start);
                end = min(data.run_end, rp->end);
                if (start < end) {
                    paddr_t sz = (end - start) + 1;
                    data.total += sz;
                    if (sz > data.contig)
                        data.contig = sz;
                }
            }
        }
        total += data.total;
        if (data.contig > contig)
            contig = data.contig;
    }
    if (totalp)
        *totalp = total;
    if (contigp)
        *contigp = contig;
}


/* Test whether a restriction range overlaps system RAM.
 *
 * Parameters:
 *   res : restriction entry to test
 * Return value: non-zero if the range overlaps a system-RAM region.
 */
int pa_sysram_overlap(struct pa_restrict *res)
{
    return (int) syscall_priv(__KSYS_PRIV_PA_SYSRAM_OVERLAP, res);
}


/* pa_init / pa_start_background — boot-time syscalls. pa_init's OUT params
 * populate the roottask-side cached values (last_paddr, restrict_proc,
 * restrict_user, system_heap_vaddr, blk_head pointer for the few roottask call
 * sites that walk it).
 */
struct kcall_args_pa_init {
    unsigned colors;
    /* OUT */
    paddr_t last_paddr_out;
    struct pa_restrict *restrict_proc_out;
    struct pa_restrict *restrict_user_out;
    struct vaddr_range system_heap_vaddr_out;
    struct block_head **blk_head_out;
};

void pa_init(unsigned colors)
{
    struct kcall_args_pa_init data;
    data.colors = colors;
    syscall_priv(__KSYS_PRIV_PA_INIT, &data);
    last_paddr = data.last_paddr_out;
    restrict_proc = data.restrict_proc_out;
    restrict_user = data.restrict_user_out;
    system_heap_vaddr = data.system_heap_vaddr_out;
    blk_head = data.blk_head_out;
}


void pa_start_background(void)
{
    syscall_priv(__KSYS_PRIV_PA_START_BACKGROUND, NULL);
}


/* Size accessors — single-syscall reads. kcall_args_pa_sizes mirrors the
 * kernel-side struct in pa.c.
 */
struct kcall_args_pa_sizes {
    memsize_t used;
    memsize_t free_unreserved;
    memsize_t reserved;
    memsize_t total;
};

memsize_t pa_free_size(void)
{
    struct kcall_args_pa_sizes s;
    syscall_priv(__KSYS_PRIV_PA_GET_SIZES, &s);
    return s.free_unreserved;
}


/* Quantum-paddr conversions. These need to walk blk_head, which lives
 * kernel-side; for roottask call sites that have a pa_quantum* we expose
 * syscall versions. pa_alloc_fake's PAQ_BLK_FAKE branch is handled inline
 * (roottask heap, no kernel walk required).
 */
paddr_t pa_quantum_to_paddr(struct pa_quantum *pq)
{
    /* Dispatch unconditionally rather than short-circuiting the PAQ_BLK_FAKE
     * case in U-mode: pq->blk lives in the kernel heap for real (non-fake)
     * quanta and would fault under PTE_U=0. The kernel-side pa_quantum_to_paddr
     * handles both cases, and its SUM=1 lets it read roottask-heap fake quanta
     * too.
     */
    return (paddr_t) syscall_priv(__KSYS_PRIV_PA_QUANTUM_TO_PADDR, pq);
}

struct pa_quantum *pa_paddr_to_quantum(paddr_t paddr)
{
    return (struct pa_quantum *) syscall_priv(__KSYS_PRIV_PA_PADDR_TO_QUANTUM,
                                              (void *) paddr);
}
