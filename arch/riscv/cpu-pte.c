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
#include <sv39-lookup.h>
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

    unsigned self = get_cpunum();
    uint64_t online = cpu_online_snapshot();
    unsigned ticket[PROCESSORS_MAX];
    bool wait[PROCESSORS_MAX];

    atomic_thread_fence(memory_order_seq_cst);
    for (int hi = 0; hi < num_processors; hi++) {
        unsigned cpu = (unsigned) hi;

        /* Skip ourselves and any CPU not yet online (cpu_online() false). A
         * not-yet-online hart cannot service an IPI to ack, so waiting on it
         * would hang early boot — a TLB flush from cpiofs_init (roottask
         * bring-up) races secondary-hart start. Mirrors the cpu_online gate in
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
        if (cpu == self || !cpu_in_mask(online, cpu)) {
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
            atomic_fetch_add_explicit(&pcpu_array[cpu].pc_tlb_flush_req, 1,
                                      memory_order_release) +
            1;
        SENDIPI(cpu, IPI_TLB_FLUSH);
    }

    for (int hi = 0; hi < num_processors; hi++) {
        unsigned cpu = (unsigned) hi;
        unsigned spins = 0;

        if (!wait[hi]) /* self, or a hart that was offline — not waited */
            continue;
        while ((int) (atomic_load_explicit(&pcpu_array[cpu].pc_tlb_flush_done,
                                           memory_order_acquire) -
                      ticket[hi]) < 0) {
            interrupt_enable();
            interrupt_disable();
            if ((++spins & 0x3ff) == 0) {
                if (!cpu_online(cpu))
                    break;
                SENDIPI(cpu, IPI_TLB_FLUSH);
            }
        }
    }
#endif
}

/* sv39_new_table — zeroed intermediate table for sv39_walk_slot.
 *
 * Comes from pa_alloc() (the vm_ops physical allocator), reached through
 * PA_TO_VA, and is marked as pa_alloc'd so cpu_pgdir_free gives it back to pa
 * rather than the kernel heap. Returns the linking PTE, or 0 on failure.
 */
static pte_t sv39_new_table(void)
{
    struct pa_quantum *pq;
    memsize_t resv = 0;
    paddr_t paddr;

    pq = pa_alloc(__PAGESIZE, __PAGESIZE, PAQ_COLOUR_NONE, 0, NULL,
                  restrict_proc, resv);
    if (!pq)
        return 0;
    pq->flags |= PAQ_FLAG_SYSTEM;

    paddr = pa_quantum_to_paddr(pq);
    CPU_ZERO_PAGE((pagetable_t) PA_TO_VA(paddr), __PAGESIZE, 0);
    return PA2PTE(paddr) | PTE_V | PTE_SW_PA;
}

/* cpu_pte_manipulate — create, modify, or remove Sv39 page table entries.
 *
 * Handles PTE_OP_MAP, PTE_OP_BAD, PTE_OP_UNMAP, PTE_OP_PROT and PTE_OP_PREALLOC
 * operations. For MAP: creates PTEs mapping [start, end) to paddr with given
 * prot. For BAD: marks the pages of [start, end) PTE_SW_BUS, past the end of
 * their object. For UNMAP: clears PTEs in [start, end). For PROT: changes
 * protection bits on existing PTEs. end is the last byte of the range (as
 * pte_map passes it) or one past it (as the kernel's own callers pass it):
 * every page that starts below end is processed, so a page-aligned end names
 * no page. See sv39_manip_pages for the errors each can return.
 *
 * Intermediate page table pages are allocated via pa_alloc() and accessed
 * through PA_TO_VA() — never assumes PA == VA.
 *
 * Progress is kept in data after every page: data->start is the first page not
 * done and, for MAP, data->paddr the frame that page gets. A call that fails
 * leaves them naming exactly how far it got, which is what lets the caller roll
 * a failed MAP back over [first, start).
 */
int cpu_pte_manipulate(struct mm_pte_manipulate *data)
{
    address_t *adp = data->adp;
    pagetable_t pgdir = adp ? adp->cpu.pgdir : pgtbl_list->pgdir;
    unsigned op = data->op;
    uint64_t bits = prot_to_pte_bits(data->prot);
    uint64_t paddr = 0; /* only a MAP supplies one */
    uintptr_t start = data->start;
    uintptr_t end = data->end;
    enum pte_op mop;
    int changed = 0;
    bool cut = false;
    int r;

    if (op & PTE_OP_MAP) {
        mop = PTE_OP_MAP;
        paddr = data->paddr;
    }
    else if (op & PTE_OP_BAD)
        mop = PTE_OP_BAD;
    else if (op & PTE_OP_UNMAP)
        mop = PTE_OP_UNMAP;
    else if (op & PTE_OP_PROT)
        mop = PTE_OP_PROT;
    else if (op & PTE_OP_PREALLOC)
        mop = PTE_OP_PREALLOC;
    else
        return EOK;

    if (mop == PTE_OP_MAP && (bits & PTE_U) && start < KERNEL_VIRT_BASE) {
        extern char _text_start[], _end[];
        uintptr_t uend = end < KERNEL_VIRT_BASE ? end : KERNEL_VIRT_BASE;
        uint64_t len = uend > start ? uend - start : 0;

        /* Guard against mapping the kernel image into a user VA. Bound the
         * window with the actual linker image extent [_text_start, _end): a
         * fixed 16 MiB window above the image crashed on a legitimate user
         * allocation whose page landed in free RAM a few MiB up (a
         * memory-pressure false positive), and deriving the base by masking a
         * function address down to 1 MiB only holds while this code stays in
         * the first megabyte of the image. section 18.
         */
        if (len && paddr < CPU_V2P((uintptr_t) _end) &&
            paddr + len > CPU_V2P((uintptr_t) _text_start)) {
            kprintf(
                "BUG: cpu_pte_manipulate mapping kernel paddr %lx "
                "into user va %lx (op=%x prot=%x)\n",
                (unsigned long) paddr, (unsigned long) start, op, data->prot);
            crash();
        }
    }

    /* Debug builds can make a MAP run dry part of the way (ENOMEM). */
    if (mop == PTE_OP_MAP)
        cut = inject_pte_map_stop(adp, start, &end);
    r = sv39_manip_pages(pgdir, mop, &start, end, &paddr, bits, sv39_new_table,
                         &changed);
    if (cut && r == EOK)
        r = ENOMEM;
    data->start = start;
    if (mop == PTE_OP_MAP)
        data->paddr = paddr;
    if (changed)
        data->tlb_flush_pending = 1;
    return r;
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

/* cpu_pte_lookup — read-only walk of pgdir for vaddr; see sv39_lookup_leaf. */
pte_t *cpu_pte_lookup(pagetable_t pgdir,
                      uintptr_t vaddr,
                      int *levelp,
                      pte_t *ep)
{
    return sv39_lookup_leaf(pgdir, vaddr, levelp, ep);
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

    int level;
    pte_t e;

    if (!sv39_lookup_leaf(pgdir, vaddr, &level, &e))
        return -1;    /* unmapped, or a leaf the hardware faults on */
    if (!(e & PTE_U)) /* user address must map a user-accessible page */
        return -1;
    *pa = (paddr_t) sv39_leaf_paddr(e, level, vaddr, NULL);
    return 0;
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
