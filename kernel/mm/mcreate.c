/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * mm_mcreate / mm_mdestroy — kernel-resident process as (un)setup invoked from
 * kext_process via vm_ops->mcreate / vm_ops->mdestroy.
 *
 * Replaces calls into as_mcreate / as_mdestroy in roottask text (unfetchable
 * from S-mode after the U-mode-roottask flip).
 *
 * What we provide:
 *   - allocate a fresh root pgdir via cpu_pgdir_create (which copies
 *     kernel L2 entries from the boot pgdir so kernel mappings are
 *     reachable when this satp is active);
 *   - allocate a address_t and link it to the process_t;
 *   - on destroy, reverse: unlink, free pgdir, free address_t.
 *
 * What we deliberately omit (handled by roottask when it owns the full mm_map
 * list of a process):
 *   - mm_map list initialization (roottask's as tracks ranges
 *     for user mmap; the kernel has no business in that bookkeeping);
 *   - rwlock init (kernel only inspects pgdir; roottask locks for
 *     its own I/O service operations);
 *   - rlimit fields (zeroed by _scalloc; roottask fills these in
 *     when it spawns processes via the loader).
 */

#include <core/globals.h>
#include <cpu-pgtable.h>
#include "as.h" /* address_t = struct mm_as */

/* Set up a fresh address space for a process.
 *
 * Allocates a root pgdir (with kernel L2 entries copied in), allocates a
 * address_t, and links it to the process. Parameters:
 *   prp : the process to set up
 * Return value: EOK on success, EINVAL/ENOMEM on failure
 */
int mm_mcreate(process_t *prp)
{
    pagetable_t pgdir;
    address_t *adp;

    if (!prp)
        return EINVAL;

    pgdir = cpu_pgdir_create();
    if (!pgdir)
        return ENOMEM;

    adp = _scalloc(sizeof(address_t));
    if (!adp) {
        cpu_pgdir_free(pgdir);
        return ENOMEM;
    }

    adp->cpu.pgdir = pgdir;
    adp->cpu.ptroot_paddr = (paddr_t) ((uintptr_t) pgdir - KERNEL_VIRT_BASE);
    adp->cpu.asid = 0;
    adp->cpu.next = NULL;
    adp->cpu.prev = NULL;

    prp->memory = adp;
    return EOK;
}

/* Tear down a process address space created by mm_mcreate.
 *
 * Unlinks the address_t from the process, frees the pgdir, and frees the
 * address_t. Parameters:
 *   prp : the process to tear down
 */
void mm_mdestroy(process_t *prp)
{
    address_t *adp;

    if (!prp || !(adp = prp->memory))
        return;

    if (adp->cpu.pgdir)
        cpu_pgdir_free((pagetable_t) adp->cpu.pgdir);

    _sfree(adp, sizeof(*adp));
    prp->memory = NULL;
}
