/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * RISC-V Sv39 PTE manipulation engine.
 *
 * These functions live in kernel arch text because they are reached from the
 * kernel-side kext_manipulate body (via do_manipulation indirection) and must
 * be S-mode-fetchable even when roottask text is marked PTE_U=1.
 *
 * Helpers: pa_alloc and pa_quantum_to_paddr live in kernel/ext/pa.c;
 * restrict_proc and pgtbl_list come from roottask via the cross-boundary table.
 */

#include <core/globals.h>
#include "as.h"
#include "mm-xboundary.h"
#include <cpu-inline.h>
#include <stdatomic.h>

/* Cross-boundary remapping for roottask-resident dependencies. */
#define restrict_proc (*kext_restrict_proc_pp)
#define pgtbl_list (*kext_pgtbl_list_pp)

/* Convert POSIX prot flags to RISC-V PTE bits. */
static uint64_t prot_to_pte_bits(unsigned prot)
{
    uint64_t bits = PTE_V | PTE_A;

    if (prot & PROT_READ)
        bits |= PTE_R;
    if (prot & PROT_WRITE)
        bits |= PTE_R | PTE_W | PTE_D;
    if (prot & PROT_EXEC)
        bits |= PTE_X;

    /* User-space mappings need PTE_U */
    bits |= PTE_U;

    return bits;
}

void cpu_tlb_flush_complete(void)
{
    sfence_vma();

#if CONFIG_PROCESSORS_MAX > 1
    if (num_processors <= 1)
        return;

    extern unsigned hart_ids[];
    unsigned self = get_cpunum();
    unsigned ticket[PROCESSORS_MAX];
    bool wait[PROCESSORS_MAX];

    atomic_thread_fence(memory_order_seq_cst);
    for (int hi = 0; hi < num_processors; hi++) {
        unsigned hart = hart_ids[hi];

        /* Skip ourselves and any hart not yet online (pc_alive == false). A
         * not-yet-online hart cannot service an IPI to ack, so waiting on it
         * would hang early boot — a TLB flush from cpiofs_init (roottask
         * bring-up) races secondary-hart start. Mirrors the pc_alive gate in
         * smp_icache_sync().
         *
         * Coherence: a hart is skippable here because it holds no TLB entry the
         * caller's page-table store could stale. It runs only
         * _smpstart/cpu_idle on the boot kernel pgdir until pc_alive, and
         * discards everything it cached during bring-up with an sfence.vma
         * immediately before publishing pc_alive (see cpu_idle). So once it is
         * alive its TLB is clean, and while it is not alive it has not touched
         * the flushed VAs.
         */
        if (hart == self ||
            !__atomic_load_n(&pcpu_array[hart].pc_alive, __ATOMIC_ACQUIRE)) {
            wait[hi] = false;
            continue;
        }
        /* Claim a ticket the target must reach with an sfence that causally
         * follows this hart's page-table store. A bare completion counter is
         * not enough: an unrelated flusher's sfence could bump it without
         * having observed our store, satisfying our wait against a stale TLB.
         * The handler acks by snapshotting pc_tlb_flush_req before its sfence,
         * so reaching our ticket proves a covering sfence ran. Track the wait
         * decision in wait[] — the ticket itself can legitimately be 0 on the
         * 2^32nd request (fetch_add wrap), so it cannot double as a sentinel.
         */
        wait[hi] = true;
        ticket[hi] =
            atomic_fetch_add_explicit(&pcpu_array[hart].pc_tlb_flush_req, 1,
                                      memory_order_release) +
            1;
        SENDIPI(hart, IPI_TLB_FLUSH);
    }

    for (int hi = 0; hi < num_processors; hi++) {
        unsigned hart = hart_ids[hi];
        unsigned spins = 0;

        if (!wait[hi]) /* self, or a hart that was offline — not waited */
            continue;
        while ((int) (atomic_load_explicit(&pcpu_array[hart].pc_tlb_flush_done,
                                           memory_order_acquire) -
                      ticket[hi]) < 0) {
            interrupt_enable();
            interrupt_disable();
            if ((++spins & 0x3ff) == 0)
                SENDIPI(hart, IPI_TLB_FLUSH);
        }
    }
#endif
}

/* sv39_walk — traverse the Sv39 page table for a virtual address.
 *
 * If alloc is true, allocates intermediate page table pages via pa_alloc() (the
 * vm_ops physical allocator). All VA↔PA conversions go through PA_TO_VA /
 * VA_TO_PA — no PA==VA assumption.
 *
 * Returns a pointer to the L0 PTE, or NULL on failure.
 */
static pte_t *sv39_walk(pagetable_t pgdir, uintptr_t va, int alloc)
{
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pgdir[PX(level, va)];
        if (*pte & PTE_V) {
            pgdir = (pagetable_t) PA_TO_VA(PTE2PA(*pte));
        } else {
            if (!alloc)
                return NULL;

            /* Allocate a new intermediate page table page */
            struct pa_quantum *pq;
            memsize_t resv = 0;
            paddr_t paddr;

            pq = pa_alloc(__PAGESIZE, __PAGESIZE, PAQ_COLOUR_NONE, 0, NULL,
                          restrict_proc, resv);
            if (!pq)
                return NULL;
            pq->flags |= PAQ_FLAG_SYSTEM;

            paddr = pa_quantum_to_paddr(pq);
            pgdir = (pagetable_t) PA_TO_VA(paddr);

            /* Zero the new page table */
            CPU_ZERO_PAGE(pgdir, __PAGESIZE, 0);

            /* Install the PDE pointing to the new page */
            *pte = PA2PTE(paddr) | PTE_V;
        }
    }
    return &pgdir[PX(0, va)];
}

/* cpu_pte_manipulate — create, modify, or remove Sv39 page table entries.
 *
 * Handles PTE_OP_MAP, PTE_OP_UNMAP, and PTE_OP_PROT operations. For MAP:
 * creates PTEs mapping [start, end) to paddr with given prot. For UNMAP: clears
 * PTEs in [start, end). For PROT: changes protection bits on existing PTEs.
 *
 * Intermediate page table pages are allocated via pa_alloc() and accessed
 * through PA_TO_VA() — never assumes PA == VA.
 */
int cpu_pte_manipulate(struct mm_pte_manipulate *data)
{
    address_t *adp = data->adp;
    pagetable_t pgdir;
    uintptr_t curr;
    unsigned op = data->op;
    uint64_t bits;

    if (adp) {
        pgdir = adp->cpu.pgdir;
    } else {
        pgdir = pgtbl_list->pgdir;
    }

    if (op & PTE_OP_BAD) {
        bits = 0;
    } else {
        bits = prot_to_pte_bits(data->prot);
    }

    for (curr = data->start; curr < data->end; curr += __PAGESIZE) {
        if (op & (PTE_OP_MAP | PTE_OP_BAD)) {
            pte_t *pte = sv39_walk(pgdir, curr, 1);
            if (!pte)
                return ENOMEM;

            if (curr < KERNEL_VIRT_BASE && (bits & PTE_U)) {
                extern char _text_start[], _end[];
                /* Guard against mapping the kernel image into a user VA. Bound
                 * the window with the actual linker image extent [_text_start,
                 * _end): a fixed 16 MiB window above the image crashed on a
                 * legitimate user allocation whose page landed in free RAM a
                 * few MiB up (a memory-pressure false positive), and deriving
                 * the base by masking a function address down to 1 MiB only
                 * holds while this code stays in the first megabyte of the
                 * image. section 18.
                 */
                if (data->paddr >= CPU_V2P((uintptr_t) _text_start) &&
                    data->paddr < CPU_V2P((uintptr_t) _end)) {
                    kprintf(
                        "BUG: cpu_pte_manipulate mapping kernel paddr %lx "
                        "into user va %lx (op=%x prot=%x)\n",
                        (unsigned long) data->paddr, (unsigned long) curr, op,
                        data->prot);
                    crash();
                }
            }

            *pte = PA2PTE(data->paddr) | bits;
            data->paddr += __PAGESIZE;
            data->tlb_flush_pending = 1;

        } else if (op & PTE_OP_UNMAP) {
            pte_t *pte = sv39_walk(pgdir, curr, 0);
            if (pte && (*pte & PTE_V)) {
                *pte = 0;
                data->tlb_flush_pending = 1;
            }

        } else if (op & PTE_OP_PROT) {
            pte_t *pte = sv39_walk(pgdir, curr, 0);
            if (pte && (*pte & PTE_V)) {
                *pte = (*pte & PTE_PADDR_BITS) | bits;
                data->tlb_flush_pending = 1;
            }

        } else if (op & PTE_OP_PREALLOC) {
            if (!sv39_walk(pgdir, curr, 1))
                return ENOMEM;
        }
    }

    return EOK;
}

/* cpu_pte_merge — merge adjacent 4 KiB PTEs into a 2 MiB megapage.
 *
 * Currently a no-op (RTMux never uses megapages); returns EOK so callers
 * proceed with 4 KiB mappings. TODO: implement for performance. Parameters:
 *   data : the manipulation descriptor (unused)
 * Return value: EOK.
 */
int cpu_pte_merge(struct mm_pte_manipulate *data)
{
    (void) data;
    return EOK;
}

/* cpu_pte_split — split a 2 MiB megapage covering vaddr into 4 KiB PTEs.
 *
 * Never reached because cpu_pte_merge never creates megapages; returns EOK as a
 * safe fallback. Parameters:
 *   vaddr : the address whose megapage would be split (unused)
 *   data  : the manipulation descriptor (unused)
 * Return value: EOK.
 */
int cpu_pte_split(uintptr_t vaddr, struct mm_pte_manipulate *data)
{
    (void) vaddr;
    (void) data;
    return EOK;
}

/* cpu_pte_mark_user — mark every existing leaf PTE in [start, end) with PTE_U
 * so U-mode CPUs can fetch/load through the kernel L2 entries.
 *
 * Used to expose roottask text/data and the syspage to U-mode pool threads.
 * Walks the current satp's page table without ever allocating intermediate
 * tables (passes alloc=0 to sv39_walk); pages outside any existing mapping are
 * silently skipped. Does sfence_vma() once at the end.
 *
 * Parameters:
 *   start : first virtual address of the range
 *   end   : one past the last virtual address of the range
 *
 * Note: the kernel L2 (and the L1/L0 tables under it) are shared across all
 * processes in RTMux (kernel-L2 prealloc). Setting PTE_U here exposes the page
 * to U-mode in every process, not just roottask.
 */
void cpu_pte_mark_user(uintptr_t start, uintptr_t end)
{
    uint64_t satp = r_satp();
    pagetable_t pgdir =
        (pagetable_t) PA_TO_VA((satp & 0xFFFFFFFFFFFUL) << PGSHIFT);

    for (uintptr_t va = start & ~(uintptr_t) (__PAGESIZE - 1); va < end;
         va += __PAGESIZE) {
        pte_t *pte = sv39_walk(pgdir, va, 0);
        if (pte && (*pte & PTE_V))
            *pte |= PTE_U;
    }
    sfence_vma();
}

/* cpu_as_vaddrinfo — report the physical mapping for a virtual address.
 *
 * Under the identity-biased mapping, the physical address is derived directly
 * from vaddr and the contiguous length runs to the end of the containing page.
 *
 * Parameters:
 *   prp    : owning process (unused — mapping is identity-biased)
 *   vaddr  : virtual address to query
 *   paddrp : if non-NULL, receives the physical address
 *   lenp   : if non-NULL, receives the contiguous length from vaddr
 * Return value: the protection bits (PROT_READ | PROT_WRITE | PROT_EXEC).
 */
unsigned cpu_as_vaddrinfo(process_t *prp,
                               uintptr_t vaddr,
                               paddr_t *paddrp,
                               size_t *lenp)
{
    (void) prp;
    if (paddrp)
        *paddrp = VA_TO_PA(vaddr);
    if (lenp)
        *lenp = __PAGESIZE - (vaddr & (__PAGESIZE - 1));
    return PROT_READ | PROT_WRITE | PROT_EXEC;
}

/* cpu_pte_lookup — read-only walk of pgdir for vaddr.
 *
 * Returns a pointer to the leaf PTE if the page is mapped, NULL otherwise.
 * Walks both 2 MiB megapages (leaf at L1) and 4 KiB pages (leaf at L0). For
 * megapage leaves, returned pointer points at the L1 entry; caller can detect
 * that case via PX(0, vaddr) versus the slot offset.
 */
pte_t *cpu_pte_lookup(pagetable_t pgdir, uintptr_t vaddr)
{
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pgdir[PX(level, vaddr)];
        if (!(*pte & PTE_V))
            return NULL;
        /* Megapage leaf at L1 (or rare L2) — has any of R/W/X. */
        if (*pte & (PTE_R | PTE_W | PTE_X))
            return pte;
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(*pte));
    }
    pte_t *pte = &pgdir[PX(0, vaddr)];
    return (*pte & PTE_V) ? pte : NULL;
}

/* cpu_uvaddr_to_paddr — physical address of a user virtual address in the
 * CURRENT process, via the running hart's page table (satp).
 *
 * Returns 0 with *pa set on success, or -1 if vaddr is out of range, unmapped,
 * or not user-accessible. Handles 4 KiB (L0), 2 MiB (L1) and 1 GiB (L2) leaves,
 * applying the page-size-correct offset for each. A MAP_SHARED page resolves to
 * the same physical address in every process mapping it, which is what lets
 * process-shared sync objects rendezvous by physical address rather than by a
 * process-local virtual address.
 *
 * Caller contract (this reads live page tables without taking a lock):
 *   - Must run in the calling user thread's own address space, so r_satp()
 *     names that process's page table — i.e. a synchronous kcall from U-mode,
 *     not a kernel/idle thread or a temporarily-switched satp.
 *   - Must ensure the address stays mapped across the walk (the page tables are
 *     not freed underneath it): the intended contended-sync caller faults the
 *     word in first and holds the address-space guard that serialises unmap
 *     (as_slock), so a concurrent munmap cannot free an intermediate table.
 *   - Rejects supervisor-only pages (no PTE_U): a stray user pointer at e.g.
 *     TRAPFRAME must not key on a supervisor physical page.
 */
int cpu_uvaddr_to_paddr(uintptr_t vaddr, paddr_t *pa)
{
    uint64_t satp = r_satp();
    pagetable_t pgdir =
        (pagetable_t) PA_TO_VA((satp & 0xFFFFFFFFFFFUL) << PGSHIFT);

    if (vaddr >= MAXVA)
        return -1;

    for (int level = 2; level >= 0; level--) {
        pte_t *pte = &pgdir[PX(level, vaddr)];

        if (!(*pte & PTE_V))
            return -1;
        /* A leaf carries R/W/X; an interior node does not. The leaf's page
         * spans 4 KiB << (9 * level), so mask the offset with PXSHIFT(level).
         */
        if (*pte & (PTE_R | PTE_W | PTE_X)) {
            uintptr_t pgmask = ((uintptr_t) 1 << PXSHIFT(level)) - 1;

            if (!(*pte & PTE_U)) /* user address must map a user-accessible page */
                return -1;
            *pa = (paddr_t) (PTE2PA(*pte) | (vaddr & pgmask));
            return 0;
        }
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(*pte));
    }
    return -1; /* walked past L0 without a leaf — not a valid mapping */
}

/* cpu_sysvaddr_find — find a system virtual address for a region.
 *
 * Under the identity mapping (VA == PA) the start address is already usable
 * as-is.
 *
 * Parameters:
 *   start : candidate start address
 *   size  : region size (unused)
 * Return value: the virtual address to use (start).
 */
uintptr_t cpu_sysvaddr_find(uintptr_t start, size_t size)
{
    /* Identity mapping: VA == PA */
    (void) size;
    return start;
}
