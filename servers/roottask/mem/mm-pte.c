/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "as.h"

/* struct pte_manip + the SPLIT/MANIPULATE/MERGE state machine (do_manipulation)
 * live in kernel/ext/manipulate.c; only the small pte_manipulate orchestrator +
 * pte_map remain roottask-side. The kcall_args blob passed through
 * __KSYS_PRIV_MANIPULATE is a "struct pte_manip" whose layout matches the
 * kernel-side definition.
 */
#define MSTATE_SPLIT_START 0

struct pte_manip {
    struct mm_pte_manipulate data;
    unsigned state;
    uintptr_t mapped;
};


/* Drive a PTE manipulation through the kernel.
 *
 * Validates page alignment, primes the manipulation state, and issues the
 * __KSYS_PRIV_MANIPULATE syscall (retrying on EINTR). Parameters:
 *   manip : the manipulation descriptor (op, range, protection, ...)
 * Return value: EOK on success, else an error code
 */
static int pte_manipulate(struct pte_manip *manip)
{
    int r;

    if (ADDR_OFFSET(manip->data.start) != 0)
        crash();
    if (ADDR_OFFSET(manip->data.end + 1) != 0)
        crash();
    if ((manip->data.op & PTE_OP_MAP) && (ADDR_OFFSET(manip->data.paddr) != 0))
        crash();

    manip->state = MSTATE_SPLIT_START;
    manip->mapped = manip->data.first = manip->data.start;
    manip->data.split_end = manip->data.end;
    manip->data.tlb_flush_pending = 0;

    /* Always go into the kernel: U-mode roottask can't manipulate PTEs
     * directly, so there is no in-kernel direct-call path.
     */
    manip->data.op |= PTE_OP_PREEMPT;
    do {
        r = (int) syscall_priv(__KSYS_PRIV_MANIPULATE, manip);
    } while (r == EINTR);

    return r;
}


/* Map a virtual address range to physical memory.
 *
 * On failure, unmaps whatever portion was already mapped. Parameters:
 *   adp   : address space
 *   start : range start VA
 *   end   : range end VA (inclusive)
 *   flags : protection flags
 *   obp   : backing object (may be NULL)
 *   paddr : physical address to map
 *   extra : extra PTE-op bits
 * Return value: EOK on success, else an error code
 */
int pte_map(address_t *adp,
            uintptr_t start,
            uintptr_t end,
            int flags,
            path_object_t *obp,
            paddr_t paddr,
            unsigned extra)
{
    struct pte_manip manip;
    int r;

    manip.data.adp = adp;
    manip.data.start = start;
    manip.data.end = end;
    manip.data.prot = (unsigned) flags;
    manip.data.paddr = paddr;
    manip.data.op = PTE_OP_MAP | extra;
    if (obp && ((obp->hdr.type == OBJECT_MEM_SHARED) ||
                (obp->hdr.type == OBJECT_MEM_TYPED))) {
        /* Keep the special-object flag synchronized for VM callers that pass a
         * populated shmem.special field.
         */
        if (obp->shmem.special != 0)
            obp->mem.mm.flags |= SHMCTL_HAS_SPECIAL;
        manip.data.shmem_flags = obp->mem.mm.flags;
        manip.data.special = obp->shmem.special;
    } else {
        manip.data.shmem_flags = 0;
    }
    r = pte_manipulate(&manip);
    if ((r != EOK) && (manip.mapped > start)) {
        /* pte_manipulate failed => need to unmap whatever's been mapped so far.
         */
        manip.data.end = manip.data.start - 1;
        manip.data.start = start;
        manip.data.op = PTE_OP_UNMAP;
        (void) pte_manipulate(&manip);
    }
    return r;
}


/* RUSH3: We're startlingly close to not needing pte_prot() anymore - just
 * RUSH3: two uses: dup.c & msync.c. Think about getting rid of it.
 */
/* Change the protection of a virtual address range.
 *
 * Parameters:
 *   adp   : address space
 *   start : range start VA
 *   end   : range end VA (inclusive)
 *   flags : new protection flags
 *   obp   : backing object
 * Return value: EOK on success, else an error code
 */
int pte_prot(address_t *adp,
             uintptr_t start,
             uintptr_t end,
             unsigned flags,
             path_object_t *obp)
{
    struct pte_manip manip;

    manip.data.adp = adp;
    manip.data.start = start;
    manip.data.end = end;
    manip.data.prot = flags;
    manip.data.op = PTE_OP_PROT;
    switch (obp->hdr.type) {
    case OBJECT_MEM_SHARED:
    case OBJECT_MEM_TYPED:
        manip.data.shmem_flags = obp->mem.mm.flags;
        manip.data.special = obp->shmem.special;
        break;
    default:
        manip.data.shmem_flags = 0;
        break;
    }
    return pte_manipulate(&manip);
}


/* Unmap a virtual address range.
 *
 * Parameters:
 *   adp   : address space
 *   start : range start VA
 *   end   : range end VA (inclusive)
 *   obp   : backing object (may be NULL)
 * Return value: EOK on success, else an error code
 */
int pte_unmap(address_t *adp,
              uintptr_t start,
              uintptr_t end,
              path_object_t *obp)
{
    struct pte_manip manip;

    manip.data.adp = adp;
    manip.data.start = start;
    manip.data.end = end;
    manip.data.op = PTE_OP_UNMAP;
    if (obp && ((obp->hdr.type == OBJECT_MEM_SHARED) ||
                (obp->hdr.type == OBJECT_MEM_TYPED))) {
        /* Keep the special-object flag synchronized for VM callers that pass a
         * populated shmem.special field.
         */
        if (obp->shmem.special != 0)
            obp->mem.mm.flags |= SHMCTL_HAS_SPECIAL;
        manip.data.shmem_flags = obp->mem.mm.flags;
        manip.data.special = obp->shmem.special;
    } else {
        manip.data.shmem_flags = 0;
    }
    return pte_manipulate(&manip);
}


/* Pre-allocate page-table entries for a virtual address range.
 *
 * Parameters:
 *   adp   : address space
 *   start : range start VA
 *   end   : range end VA (inclusive)
 * Return value: EOK on success, else an error code
 */
int pte_prealloc(address_t *adp, uintptr_t start, uintptr_t end)
{
    struct pte_manip manip;

    manip.data.adp = adp;
    manip.data.start = start;
    manip.data.end = end;
    manip.data.op = PTE_OP_PREALLOC;
    manip.data.shmem_flags = 0;
    return pte_manipulate(&manip);
}


/* Mark the page containing vaddr as bad/inaccessible.
 *
 * Parameters:
 *   adp   : address space
 *   vaddr : virtual address within the page to mark
 * Return value: EOK on success, else an error code
 */
int pte_bad(address_t *adp, uintptr_t vaddr)
{
    struct pte_manip manip;

    manip.data.adp = adp;
    vaddr = ADDR_PAGE(vaddr);
    manip.data.start = vaddr;
    manip.data.end = vaddr | (__PAGESIZE - 1);
    manip.data.op = PTE_OP_BAD;
    manip.data.prot = 0;
    manip.data.shmem_flags = 0;
    manip.data.paddr = 0;
    return pte_manipulate(&manip);
}
