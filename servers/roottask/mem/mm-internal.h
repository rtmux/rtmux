/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _MM_INTERNAL_H
#define _MM_INTERNAL_H

#ifdef KERNEL_INTERNAL
#include <core/globals.h>
#else
#include "externs.h"
#endif

#ifndef EXTERN
#define EXTERN extern
#endif

/* General utility macros */
#define VA_INVALID ((uintptr_t) - 1)
#define MM_MAP_PRIVATEANON 0x00000003U
#define MM_MAP_NOX64K 0x00020000
#define MM_MAP_BELOW16M 0x00040000
#define MM_UNMAP_INIT_REQUIRED 0x00000001U
#define MM_UNMAP_INIT_OPTIONAL 0x00000002U

#define SHMCTL_ANON 0x00000001 /* Allocate anon memory for object */
#define SHMCTL_PHYS 0x00000002 /* Providing physical memory for object */
#define SHMCTL_ISADMA                                                        \
    0x00000004                   /* ISA DMA hint; x86 constraints no-op here \
                                  */
#define SHMCTL_GLOBAL 0x00000010 /* Hint that object is globally visible */
#define SHMCTL_PRIV 0x00000020   /* Accessing object requires privilege */
#define SHMCTL_NOX64K 0x00000100 /* Memory should not cross 64K boundary */
#define SHMCTL_HIGHUSAGE 0x00000200 /* Prefer limited high-usage resources */
#define SHMCTL_LAZY 0x00000400      /* Delay allocation until first reference */
#define SHMCTL_HAS_SPECIAL 0x00008000
#define SHMCTL_FLAG_MASK 0x0000ffff

#define ADDR_OFFSET_BITS 12
#define ADDR_OFFSET_MASK ((1UL << ADDR_OFFSET_BITS) - 1)
#define ADDR_OFFSET(a) ((uintptr_t) (a) & ADDR_OFFSET_MASK)
#define ADDR_PAGE(a) ((a) & ~ADDR_OFFSET_MASK)

#if (__PAGESIZE - 1) != ADDR_OFFSET_MASK
#error __PAGESIZE and ADDR_OFFSET_BITS do not agree
#endif

/* RTMux is LP64-only: _PADDR_BITS is always 64 */
#if !defined(_PADDR_BITS) || _PADDR_BITS < 64
#error _PADDR_BITS must be 64 on RTMux (check arch/<cpu>/include/platform.h)
#endif

#define HIGH_PADDR(paddr) ((paddr) >> 32)
#define LOW_PADDR(paddr) ((uint32_t) (paddr))

#define VPS_NONE 0
#define VPS_AUTO 1
#define VPS_HIGHUSAGE 2

#include "cpu-mm-internal.h"

/* Cache coloring is not needed on the current RISC-V target. */
#define COLOUR_ADJUST_PA(paddr, paddr_colour) (paddr)

/* Flags for root_vm_ops.validate() */
#define VV_RANGE 0x1
#define VV_MAPPED 0x2


struct vaddr_range {
    uintptr_t lo;
    uintptr_t hi;
};

#define MM_FLAG_MULTIPLE_DCACHE_LEVELS 0x0002
#define MM_FLAG_ENFORCE_ALIGNMENT 0x0004
#define MM_FLAG_VPS 0x0020

EXTERN struct pa_restrict *restrict_proc;
EXTERN struct pa_restrict *restrict_user;
EXTERN paddr_t last_paddr;
EXTERN uintptr_t va_rover;
EXTERN unsigned mm_flags;

/* address_cookie removed: address_t is now separately allocated, not embedded
 * behind kernel-managed process objects.
 */
EXTERN struct vaddr_range system_heap_vaddr;
EXTERN uintptr_t pgszlist[sizeof(uintptr_t) * 8 + 1];

#include "pa.h"

void pa_init(unsigned colors);
void pa_start_background(void);


/* Physical Allocation flags (input) */
#define PAA_FLAG_CONTIG 0x01
#define PAA_FLAG_NOX64K 0x02
#define PAA_FLAG_HIGHUSAGE 0x04

/* Physical Allocation Status flags (output) */
#define PAA_STATUS_NOT_ZEROED 0x01
#define PAA_STATUS_COLOUR_MISMATCH 0x02
#define PAA_STATUS_ISCONTIG 0x04

struct pa_quantum *pa_alloc(paddr_t size,
                            paddr_t align,
                            unsigned color,
                            unsigned flags,
                            unsigned *status,
                            struct pa_restrict *restriction,
                            paddr_t resv_size);
int pa_alloc_given(struct pa_quantum *, size_t, unsigned *);
void pa_free(struct pa_quantum *paq, size_t num, paddr_t resv_size);
void pa_free_list(struct pa_quantum *paq, paddr_t resv_size);
void pa_free_paddr(paddr_t p, paddr_t s, paddr_t resv_size);
struct pa_quantum *pa_alloc_fake(paddr_t paddr, paddr_t size);
void pa_free_fake(struct pa_quantum *paq);
void pa_free_info(struct pa_restrict *restriction,
                  paddr_t *total,
                  paddr_t *contig);
int pa_sysram_overlap(struct pa_restrict *restriction);
paddr_t pa_quantum_to_paddr(struct pa_quantum *pq);
struct pa_quantum *pa_paddr_to_quantum(paddr_t paddr);

memsize_t pa_free_size(void);

extern int shmem_create(path_object_t *, void *);
extern int shmem_done(path_object_t *);

extern struct pa_quantum *tymem_rdb_alloc(path_object_t *obp,
                                          size_t size,
                                          unsigned flags);
extern struct pa_quantum *tymem_rdb_alloc_given(path_object_t *obp,
                                                paddr_t paddr,
                                                size_t size,
                                                int *r);
extern void tymem_rdb_free(path_object_t *obp, paddr_t paddr, size_t size);
extern void tymem_free_info(path_object_t *obp,
                            paddr_t *total,
                            paddr_t *contig);

extern void rdecl memobj_lock(path_object_t *);
extern int rdecl memobj_cond_lock(path_object_t *);
extern void rdecl memobj_unlock(path_object_t *);


/* path_object_t embeds mm_object so the path service and VM service share one
 * namespace object. Keep the definition in path-object.h until ownership is
 * split.
 */

#include "path-object.h"

/* Flags for memory objects */

/* Global flags */
#define MM_MEM_HAS_SYNC 0x80000000

#define MM_MEM_SKIPLOCKCHECK 0x20000000
#define MM_MEM_RDB 0x08000000
#define MM_MEM_HAS_BAD_PAGE 0x04000000

/* Shared memory flags reuse SHMCTL_FLAG_* bits */
#define MM_SHMEM_SPECIAL (SHMCTL_FLAG_MASK + 1)
#define MM_SHMEM_IFS (MM_SHMEM_SPECIAL << 1)

/* mmap'd file flags */
#define MM_FDMEM_STICKY 0x00000001
#define MM_FDMEM_NEEDSSYNC 0x00000008

/* Anonymous, typed memory flags reuse POSIX_TYPED_MEM_* bits */
#define MM_ANMEM_TYPED_MASK 0x0000000f
#define MM_ANMEM_MULTI_REFS 0x00000010

#define VERIFY_OBJ_LOCK(o)                                    \
    CRASHCHECK(!((o)->mem.mm.flags & MM_MEM_SKIPLOCKCHECK) && \
               !proc_mux_haslock(&(o)->mem.mm.mux, 0))

#endif /* _MM_INTERNAL_H */
