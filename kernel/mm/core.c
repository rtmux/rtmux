/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Kernel-resident VM hooks.
 *
 * The kernel cannot fetch from as_* in U-mode roottask text (PTE_U=1; S-mode
 * fetch from PTE_U=1 is illegal regardless of SUM). The kernel needs its OWN,
 * permanent vm_ops — every vm_ops->FOO callsite must land in S-mode-fetchable
 * kernel text. This file (and the rest of kernel/mm/) is that vm_ops.
 */

#include <unistd.h>
#include <core/globals.h>
#include <sys/mman.h>

/* Boot heap allocated by startup via calloc_ram(). Defined in startup/main.c;
 * excluded from sysram asinfo entries.
 */
extern paddr_t boot_heap_paddr;
extern size_t boot_heap_size;

/* Forward decl — definition follows below in this file. */
int mm_pmem_add(paddr_t start, paddr_t len);

/* Initialize kernel-resident memory management.
 *
 * Parameters:
 *   phase : initialization phase selector (phase 0 seeds the kernel heap)
 */
void mm_init_mem(int phase)
{
    if (phase == 0) {
        /* Seed the kernel heap with the boot heap region pre-allocated by
         * startup. This memory is NOT in sysram, so pa_init (which runs later
         * in roottask) won't try to manage it.
         */
        LOGF_D("mm_init_mem: boot_heap PA=%lx size=%lx\n",
               (unsigned long) boot_heap_paddr, (unsigned long) boot_heap_size);
        mm_pmem_add(boot_heap_paddr, boot_heap_size);
    }
}

/* Allocator callback that yields nothing.
 *
 * Used as the grow callback for the kernel heap: all available memory has
 * already been placed in the heap, so the system has nothing more to hand out.
 * Parameters:
 *   size : requested size (ignored)
 *   addr : out pointer for the new region (ignored)
 * Return value: always 0 (no memory provided)
 */
static size_t _dummy_alloc(size_t size, void **addr)
{
    /* Can't get anything more from the system, since it's all been put in the
     * heap.
     */
    return 0;
}

/* Map memory for a process.
 *
 * Handles anonymous allocations (MAP_ANON, satisfied from the kernel heap) and
 * physical pass-through mappings (MAP_PHYS | MAP_SHARED, where physical ==
 * virtual). Parameters:
 *   pid    : owning process id (unused)
 *   prp    : owning process
 *   addr   : requested virtual address hint
 *   len    : length of the mapping
 *   prot   : protection flags
 *   flags  : mapping flags (MAP_ANON, MAP_PHYS, MAP_SHARED, MAP_STACK, ...)
 *   obpe   : backing path namespace object
 *   boff   : backing object offset
 *   align  : requested alignment
 *   preload: preload size
 *   fd     : backing file descriptor
 *   vaddr  : out pointer for the resulting virtual address
 *   size   : out pointer for the resulting size
 * Return value: EOK on success, EINVAL/ENOMEM on failure
 */
int mm_mmap(pid_t pid,
            process_t *prp,
            uintptr_t addr,
            size_t len,
            int prot,
            int flags,
            path_object_t *obpe,
            uint64_t boff,
            size_t align,
            size_t preload,
            int fd,
            void **vaddr,
            size_t *size)
{
    uint64_t off = boff;
    (void) pid;

    /* NYI: Doesn't handle PROT_NOCACHE Physical can't do guardpages, so remove
     * them from the len
     */
    if (flags & MAP_STACK) {
        if (off >= len) {
            return EINVAL;
        }
        len -= off;
    }
    /* Allocate memory */
    if (flags & MAP_ANON) {
        /* Round up to a page. Compute the mask in size_t: vm_ops->pagesize is a
         * 32-bit unsigned, so ~(pagesize - 1) would be a 32-bit mask that
         * silently truncates any len >= 4 GiB. Also reject a length whose
         * round-up would wrap to a tiny allocation.
         */
        size_t page_mask = (size_t) vm_ops->pagesize - 1;

        if (len > SIZE_MAX - page_mask)
            return EINVAL;
        len = (len + page_mask) & ~page_mask;
        for (;;) {
            *vaddr = _sreallocfunc(0, 0, len, _dummy_alloc, HEAP_KER);
            if (*vaddr)
                break;
            if (!purger_invoke(len)) {
                return ENOMEM;
            }
        }
        *size = len;
        return EOK;
    }
    /* since physical == virtual, return offset */
    if ((flags & (MAP_PHYS | MAP_TYPE)) == (MAP_PHYS | MAP_SHARED)) {
        *vaddr = (void *) CPU_P2V(off);
        *size = len;
        return EOK;
    }

    return EINVAL;
}

/* Unmap a region previously mapped by mm_mmap.
 *
 * Parameters:
 *   pid   : owning process id (unused)
 *   prp   : owning process
 *   addr  : virtual address of the region
 *   len   : length of the region
 *   flags : unmap flags
 * Return value: EOK on success
 */
int mm_munmap(pid_t pid, process_t *prp, uintptr_t addr, size_t len, int flags)
{
    (void) pid;
    memobj_destroyed((void *) _syspage_ptr, CPU_V2P(addr),
                     CPU_V2P(addr) + len - 1, 0, 0);
    _sfree((void *) addr, len);
    return EOK;
}

#define MEM_ALIGNMENT (sizeof(uint64_t))

/* Add a physical memory region to the kernel heap.
 *
 * The region is rounded inward to MEM_ALIGNMENT before being freed into the
 * heap. Parameters:
 *   start : physical start address
 *   len   : length of the region
 * Return value: EOK
 */
int mm_pmem_add(paddr_t start, paddr_t len)
{
    uintptr_t addr;
    uintptr_t end;

    /* adjust to make sure addr and size fits within a page */
    addr = ((uintptr_t) start + MEM_ALIGNMENT - 1) & ~(MEM_ALIGNMENT - 1);
    end = ((uintptr_t) (start + len) & ~(MEM_ALIGNMENT - 1));

    if ((end > addr) && (end != 0)) {
        _sfree((void *) CPU_P2V(addr), end - addr);
    }
    return EOK;
}

/* Resolve a virtual address to its backing memory object and offset.
 *
 * Since physical == virtual in the kernel-resident vm_ops, the offset is simply
 * the physical address and the object is the syspage. Parameters:
 *   prp    : owning process (unused)
 *   vaddr  : virtual address to resolve
 *   offset : out pointer for the physical offset
 *   create : create-if-absent flag (unused)
 * Return value: the backing path namespace object
 */
path_object_t *mm_vaddr_to_memobj(process_t *prp,
                                  void *vaddr,
                                  paddr_t *offset,
                                  int create)
{
    *offset = CPU_V2P(vaddr);
    return (void *) _syspage_ptr;
}
