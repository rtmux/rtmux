/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * roottask-view.c — install/remove a read-only view of roottask's text + data
 * into a child process's pgdir, so a loader thread that runs in the child's as
 * can still execute roottask's loader code and touch roottask's globals
 * (vm_ops, file table, allocators).
 *
 * Why this exists:
 *   The U-mode roottask image is placed at RTMUX_ROOTTASK_UADDR_BASE
 *   (Sv39 L2[3] = 0xC0000000-0xFFFFFFFF), private to roottask's pgdir.
 *   When roottask spawns a child it creates a "loader thread" inside
 *   the child's process_t.  That thread runs roottask's loader_load()
 *   to populate the child's image — but it must (a) reach roottask's
 *   text to execute, (b) reach roottask's globals to use the vm_ops
 *   and file-table state, and (c) write into the child's freshly
 *   mmap'd PT_LOAD buffers.
 *
 *   Doing all three in one pgdir requires the child's pgdir to map
 *   BOTH the child's own pages AND roottask's image.  This file
 *   provides the install/uninstall pair; the install runs at loader-
 *   thread creation, the uninstall runs in kext_process_startup
 *   immediately before satp-switch + sret to U-mode, leaving the
 *   child fully isolated from roottask once it runs user code.
 *
 *   We share roottask's L1 page (one-PTE copy at the L2[3] slot), so
 *   the install is a single store and uninstall is a single clear.
 *   The shared L1 means any future mmap from the child into the
 *   reserved range would clobber roottask — that's why the loader
 *   refuses PT_LOAD whose vaddr falls in [ROOTTASK_UADDR_BASE, _END)
 *   (see servers/roottask/sys/loader-elf.c).
 *
 *   cpu_pgdir_free's user-half walk is already limited to L2[0], so
 *   destroying a failed-load child pgdir does not touch (and cannot
 *   free) the shared L1 page.  No unmap-on-error path needed.
 */

#include <core/globals.h>
#include "as.h"         /* address_t = struct mm_as */
#include <cpu-paging.h> /* RTMUX_ROOTTASK_UADDR_BASE, PX(), pte_t */

/* Sv39 L2 slot for the roottask reservation. RTMUX_ROOTTASK_UADDR_BASE =
 * 0xC0000000 → L2 index 3.
 */
#define ROOTTASK_L2_SLOT PX(2, RTMUX_ROOTTASK_UADDR_BASE)

/* Map roottask's image into target_prp's pgdir.
 *
 * Returns EOK on success. Errors: EINVAL (bad target), ENOMEM (roottask not yet
 * up), EBUSY (slot already populated — the caller should treat this as a logic
 * error: only one loader thread per child at a time).
 */
int mm_roottask_view_install(process_t *target_prp)
{
    process_t *root_prp;
    pagetable_t root_pgdir, child_pgdir;

    if (!target_prp || !target_prp->memory)
        return EINVAL;

    root_prp = roottask_prp;
    if (!root_prp || !root_prp->memory)
        return ENOMEM;

    root_pgdir = (pagetable_t) root_prp->memory->cpu.pgdir;
    child_pgdir = (pagetable_t) target_prp->memory->cpu.pgdir;
    if (!root_pgdir || !child_pgdir)
        return EINVAL;

    if (child_pgdir[ROOTTASK_L2_SLOT] != 0)
        return EBUSY;

    child_pgdir[ROOTTASK_L2_SLOT] = root_pgdir[ROOTTASK_L2_SLOT];

    /* The TLB doesn't yet hold any translation for this VA in the child's as
     * (no thread of this child has run with this satp referring to this slot),
     * so no sfence.vma is required here. The first dispatch to a thread in this
     * as will see the new entry on demand.
     */
    return EOK;
}

/* Remove roottask's view from target_prp's pgdir. Idempotent. Must be invoked
 * with the kernel BKL held; the caller is kext_process_startup just before the
 * satp-switch to the child.
 */
void mm_roottask_view_remove(process_t *target_prp)
{
    pagetable_t child_pgdir;

    if (!target_prp || !target_prp->memory)
        return;

    child_pgdir = (pagetable_t) target_prp->memory->cpu.pgdir;
    if (!child_pgdir)
        return;

    if (child_pgdir[ROOTTASK_L2_SLOT] == 0)
        return;

    child_pgdir[ROOTTASK_L2_SLOT] = 0;

    /* sfence.vma here covers the case where another CPU might still be holding
     * TLB entries for the roottask range under this satp. In practice the only
     * thread currently bound to this as is the loader thread which is about to
     * satp-switch away (we're called from kext_process_startup, which then
     * dispatches the thread to U-mode in the child's now-clean as), so there
     * are no live mappings to invalidate elsewhere — but the fence is cheap and
     * the flush avoids stale-TLB surprises if the dispatch model ever changes.
     */
    __asm__ volatile("sfence.vma" ::: "memory");
}
