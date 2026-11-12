/* SPDX-License-Identifier: Apache-2.0 */

#include <core/globals.h>
#include "as.h"
#include "mm-xboundary.h"
#include <cpu-inline.h>
#include <stdatomic.h>

#define restrict_proc (*kext_restrict_proc_pp)
#define pgtbl_list (*kext_pgtbl_list_pp)

static uint64_t prot_to_pte_bits(unsigned prot)
{
    uint64_t bits = PTE_V | PTE_A;

    if (prot & PROT_READ)
        bits |= PTE_R;
    if (prot & PROT_WRITE)
        bits |= PTE_R | PTE_W | PTE_D;
    if (prot & PROT_EXEC)
        bits |= PTE_X;

    bits |= PTE_U;

    return (prot & (PROT_READ | PROT_WRITE | PROT_EXEC))
               ? aarch64_page_attrs((unsigned) bits)
               : 0;
}

void cpu_tlb_flush_complete(void)
{
    cpu_tlb_flush();
}

static pte_t *aarch64_walk(pagetable_t pgdir, uintptr_t va, int alloc)
{
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pgdir[PX(level, va)];
        if (*pte & PTE_V) {
            pgdir = (pagetable_t) PA_TO_VA(PTE2PA(*pte));
        } else {
            if (!alloc)
                return NULL;

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

            CPU_ZERO_PAGE(pgdir, __PAGESIZE, 0);

            *pte = PA2PTE(paddr) | 3;
        }
    }
    return &pgdir[PX(0, va)];
}

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
            pte_t *pte = aarch64_walk(pgdir, curr, 1);
            if (!pte)
                return ENOMEM;

            if (curr < KERNEL_VIRT_BASE && (bits & (1UL << 6))) {
                extern char _text_start[], _end[];

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

            if (*pte & PTE_V) {
                *pte = 0;
                cpu_tlb_flush();
            }
            *pte = bits ? PA2PTE(data->paddr) | bits : 0;
            data->paddr += __PAGESIZE;
            data->tlb_flush_pending = 1;

        } else if (op & PTE_OP_UNMAP) {
            pte_t *pte = aarch64_walk(pgdir, curr, 0);
            if (pte && (*pte & PTE_V)) {
                *pte = 0;
                data->tlb_flush_pending = 1;
            }

        } else if (op & PTE_OP_PROT) {
            pte_t *pte = aarch64_walk(pgdir, curr, 0);
            if (pte && (*pte & PTE_V)) {
                pte_t old = *pte;
                *pte = 0;
                cpu_tlb_flush();
                *pte = bits ? (old & PTE_PADDR_BITS) | bits : 0;
                data->tlb_flush_pending = 1;
            }

        } else if (op & PTE_OP_PREALLOC) {
            if (!aarch64_walk(pgdir, curr, 1))
                return ENOMEM;
        }
    }

    return EOK;
}

int cpu_pte_merge(struct mm_pte_manipulate *data)
{
    (void) data;
    return EOK;
}

int cpu_pte_split(uintptr_t vaddr, struct mm_pte_manipulate *data)
{
    (void) vaddr;
    (void) data;
    return EOK;
}

void cpu_pte_mark_user(uintptr_t start, uintptr_t end)
{
    pagetable_t pgdir = (pagetable_t) PA_TO_VA(cpu_page_table_root());

    for (uintptr_t va = start & ~(uintptr_t) (__PAGESIZE - 1); va < end;
         va += __PAGESIZE) {
        pte_t *pte = aarch64_walk(pgdir, va, 0);
        if (pte && (*pte & PTE_V))
            *pte |= 1UL << 6;
    }
    cpu_tlb_flush();
}

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

pte_t *cpu_pte_lookup(pagetable_t pgdir, uintptr_t vaddr)
{
    for (int level = 2; level > 0; level--) {
        pte_t *pte = &pgdir[PX(level, vaddr)];
        if (!(*pte & PTE_V))
            return NULL;

        if (cpu_pte_leaf(*pte, level))
            return pte;
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(*pte));
    }
    pte_t *pte = &pgdir[PX(0, vaddr)];
    return (*pte & PTE_V) ? pte : NULL;
}

int cpu_uvaddr_to_paddr(uintptr_t vaddr, paddr_t *pa)
{
    pagetable_t pgdir = (pagetable_t) PA_TO_VA(cpu_page_table_root());

    if (vaddr >= MAXVA)
        return -1;

    for (int level = 2; level >= 0; level--) {
        pte_t *pte = &pgdir[PX(level, vaddr)];

        if (!(*pte & PTE_V))
            return -1;

        if (cpu_pte_leaf(*pte, level)) {
            uintptr_t pgmask = ((uintptr_t) 1 << PXSHIFT(level)) - 1;

            if (!(*pte & (1UL << 6)))
                return -1;
            *pa = (paddr_t) (PTE2PA(*pte) | (vaddr & pgmask));
            return 0;
        }
        pgdir = (pagetable_t) PA_TO_VA(PTE2PA(*pte));
    }
    return -1;
}

uintptr_t cpu_sysvaddr_find(uintptr_t start, size_t size)
{
    (void) size;
    return start;
}
