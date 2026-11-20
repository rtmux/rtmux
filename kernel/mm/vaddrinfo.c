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
#include "as.h"          /* CPU_1TO1_VADDR_BIAS, PA_TO_VA */
#include <sv39-lookup.h> /* PTE_*, cpu_pte_lookup, sv39_leaf_paddr */
#include <sys/mman.h>

/* mm_vaddrinfo — translate a virtual address to its physical mapping.
 *
 * Walks the target process's Sv39 page table (kernel-direct-map VAs take a fast
 * path) and derives a POSIX prot bitmap from the leaf PTE bits. Parameters:
 *   pid    : owning process pid (unused; translation uses prp)
 *   prp    : process whose page table is walked
 *   vaddr  : virtual address to translate
 *   paddrp : if non-NULL, receives the physical address
 *   lenp   : if non-NULL, receives the bytes from vaddr to the end of the
 *            leaf that maps it (4 KiB, 2 MiB or 1 GiB page)
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

    /* Use the entry as the walk checked it, so prot and paddr describe one leaf
     * even if a concurrent unmap or remap changes it afterwards.
     */
    int level;
    pte_t pv;
    if (!cpu_pte_lookup(pgdir, vaddr, &level, &pv))
        return PROT_NONE;

    /* Translate PTE bits → POSIX prot bits. PTE_U doesn't matter for the caller
     * (kernel S-mode access uses SUM); we report read/write/exec presence.
     */
    if (pv & PTE_R)
        prot |= PROT_READ;
    if (pv & PTE_W)
        prot |= PROT_WRITE;
    if (pv & PTE_X)
        prot |= PROT_EXEC;

    /* The leaf's level fixes how many low VA bits are the offset into it: 12
     * for 4 KiB, 21 for a 2 MiB megapage, 30 for a 1 GiB gigapage.
     */
    paddr_t paddr = (paddr_t) sv39_leaf_paddr(pv, level, vaddr, lenp);
    if (paddrp)
        *paddrp = paddr;

    return prot;
}
