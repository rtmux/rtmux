/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AArch64 pte_temp_map -- always use kernel 1:1 direct-map VA.
 *
 * On AArch64, the kernel's direct map (KERNEL_VIRT_BASE + PA) covers all
 * physical RAM and is accessible from EL1 regardless of which user-space
 * page table is loaded in TTBR0_EL1. This means we never need to create
 * temporary user-space PTE mappings to initialize pages -- we can always reach
 * the physical pages through their kernel VA.
 *
 * The generic mm-temp-map.c creates temp PTEs in the target process's address
 * space, which breaks when the kernel (EL1) runs with a different TTBR0_EL1
 * than the target. This override avoids the problem entirely.
 */

#include "as.h"

int pte_temp_map(address_t *adp,
                 uintptr_t user_va,
                 struct pa_quantum *pq,
                 struct mm_map *mm,
                 size_t len,
                 int (*func)(void *, size_t, void *),
                 void *d)
{
    paddr_t paddr;
    uintptr_t vaddr;

    (void) adp;
    (void) user_va;

    paddr = pa_quantum_to_paddr(pq);
    vaddr = (uintptr_t) paddr + CPU_1TO1_VADDR_BIAS;

    return func((void *) vaddr, len, d);
}
