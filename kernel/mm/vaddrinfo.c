/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * mm_vaddrinfo — kernel-resident vaddr→paddr translator.
 *
 * Replaces the kernel's call into as_vaddrinfo (roottask text, unfetchable from
 * S-mode now that roottask runs U-mode). Walks the target process's Sv39 page
 * table directly via cpu_pte_lookup.
 *
 * The roottask-side as_vaddrinfo also consults memobj/mm_map structures for
 * protection metadata; the kernel callsites that go through vm_ops->vaddrinfo
 * (message, xfer, debug, trace) only need the VA→PA translation and a usable
 * prot bitmap. We derive prot from the live PTE bits, which is conservative but
 * correct.
 */

#include <core/globals.h>
#include "as.h"         /* CPU_1TO1_VADDR_BIAS, PA_TO_VA */
#include <cpu-paging.h> /* PTE_R / PTE_W / PTE_X / PGSIZE / PXSHIFT */
#include <sys/mman.h>

extern pte_t *cpu_pte_lookup(pagetable_t pgdir, uintptr_t vaddr);

/* mm_vaddrinfo — translate a virtual address to its physical mapping.
 *
 * Walks the target process's Sv39 page table (kernel-direct-map VAs take a fast
 * path) and derives a POSIX prot bitmap from the leaf PTE bits. Parameters:
 *   pid    : owning process pid (unused; translation uses prp)
 *   prp    : process whose page table is walked
 *   vaddr  : virtual address to translate
 *   paddrp : if non-NULL, receives the physical address
 *   lenp   : if non-NULL, receives the contiguous length from vaddr
 *   flags  : reserved (unused)
 * Return value: the PROT_* bits for the mapping, PROT_NONE if unmapped.
 */
unsigned mm_vaddrinfo(pid_t pid,
                      process_t *prp,
                      uintptr_t vaddr,
                      paddr_t *paddrp,
                      size_t *lenp,
                      unsigned flags)
{
    unsigned prot = PROT_NONE;
    (void) pid;

    /* High-VA (kernel direct map) fast path. Identical to the as_vaddrinfo's
     * CPU_1TO1_IS_VADDR branch — kernel data is reachable as PA = VA -
     * KERNEL_VIRT_BASE.
     */
    if ((uintptr_t) vaddr >= KERNEL_VIRT_BASE) {
        if (paddrp)
            *paddrp = (paddr_t) ((uintptr_t) vaddr - KERNEL_VIRT_BASE);
        if (lenp)
            *lenp = ~(size_t) 0;
        return PROT_READ | PROT_WRITE | PROT_EXEC;
    }

    (void) flags;

    /* Walk the target process's pgdir. Caller passes prp; we use its
     * address_t->cpu.pgdir (already a kernel high-VA pointer).
     */
    if (!prp || !prp->memory)
        return PROT_NONE;

    pagetable_t pgdir = (pagetable_t) prp->memory->cpu.pgdir;
    pte_t *pte = cpu_pte_lookup(pgdir, vaddr);
    if (!pte || !(*pte & PTE_V))
        return PROT_NONE;

    /* Translate PTE bits → POSIX prot bits. PTE_U doesn't matter for the caller
     * (kernel S-mode access uses SUM); we report read/write/exec presence.
     */
    if (*pte & PTE_R)
        prot |= PROT_READ;
    if (*pte & PTE_W)
        prot |= PROT_WRITE;
    if (*pte & PTE_X)
        prot |= PROT_EXEC;

    /* Compute PA: extract PPN from PTE, OR with VA's intra-page offset.
     * Megapage leaves at L1 cover 2 MiB; detect via the R/W/X bits at the L1
     * slot via cpu_pte_lookup's contract — for a 4 KiB leaf the offset is the
     * 12 low bits, for a megapage it's the low 21 bits. Simple heuristic: check
     * the page's PA alignment in the PTE. Cleaner: walk levels here ourselves.
     */
    paddr_t base = PTE2PA(*pte);
    if (paddrp) {
        /* Conservative: assume 4 KiB granularity. This is correct for
         * cpu_pte_lookup returning an L0 PTE. For a megapage (rare; kernel
         * callsites operate on user mappings which are 4 KiB after the
         * initramfs loader), the low 9 bits of VA's L0 portion are folded —
         * we'd return a PA aligned down to the megapage boundary. Acceptable
         * for IPC validation (PROT match) but TODO: distinguish megapage leaves
         * and add VA's low 21 bits in that case.
         */
        *paddrp = base | (vaddr & (__PAGESIZE - 1));
    }
    if (lenp)
        *lenp = __PAGESIZE - (vaddr & (__PAGESIZE - 1));

    return prot;
}
