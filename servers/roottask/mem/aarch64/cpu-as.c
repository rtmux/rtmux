/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AArch64 Sv39 VMM cpu-dependent functions.
 *
 * Manages per-process root page tables and the global page table list. All
 * VA/PA conversions go through PA_TO_VA / VA_TO_PA — no assumption that PA ==
 * VA.
 */

#include "as.h"
#include "externs.h" /* syscall_priv, __KSYS_PRIV_* */
#include <paging.h>
#include <cpu-inline.h>

/* Doubly-linked list of all active per-process page tables */
struct cpu_mm_as *pgtbl_list;

/* Dummy as used until ROOTTASK_PID's real one is allocated */
static struct cpu_mm_as dummy_as;

/* Page table operations */

/* pgtbl_init — initialize the dummy address space from the boot pgdir PA.
 *
 * Called during as_init_mem phase 0 to capture the kernel's root page table as
 * the template for kernel-half entries in later per-process pgdirs.
 *
 * EL0 roottask cannot execute "mrs TTBR0_EL1" (illegal in U), so the root pgdir
 * PA comes via __KSYS_PRIV_GET_KERN_PGDIR. The kernel-side handler returns
 * "page_directory" (boot CPU pgdir). All kernel L2 entries are shared across
 * pgdirs so this is interchangeable with roottask's own pgdir for the dummy_as
 * template purpose.
 */
void pgtbl_init(void)
{
    paddr_t kern_ptroot =
        (paddr_t) syscall_priv(__KSYS_PRIV_GET_KERN_PGDIR, NULL);

    dummy_as.pgdir = (void *) kern_ptroot; /* VA == PA */
    dummy_as.ptroot_paddr = kern_ptroot;
    dummy_as.asid = 0;
    pgtbl_list = &dummy_as;
}

/* PTE manipulation lives in arch/aarch64/cpu-pte.c. */

/* cpu_as_vaddrinfo — roottask-side virtual-address info helper.
 *
 * The kernel has its own implementation in arch/aarch64/cpu-pte.c for
 * privileged callbacks. This copy is linked into roottask and used by roottask
 * VM paths.
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
