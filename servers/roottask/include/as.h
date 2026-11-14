/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ROOTTASK_AS_H
#define _ROOTTASK_AS_H

#include "mm-internal.h"

/* Address-space data structures */

/* struct mm_map_head moved to mm-map-head.h so root-process.h can hold one by
 * value without dragging the whole of as.h in.
 */
#include "mm-map-head.h"

struct map_set {
    struct mm_map_head *head;
    struct mm_map *first;
    struct mm_map *last;
    struct mm_map *prev;
    int flags;
};

struct mm_object_ref {
    struct mm_object_ref *next;
    struct mm_as *adp;

    /* Pid of the process owning "adp". Set at memref_add time so U-mode
     * roottask doesn't have to deref adp->owner (gone post-split) to discover
     * which root_process_t slot to consult.
     */
    pid_t pid;
    path_object_t *obp;
    struct mm_map *first_ref;
    int fd;
};


#define EXTRA_FLAG_MADV_MASK 0x000f /* bottom 4 bits are madvise advice */
#define EXTRA_FLAG_RDONLY 0x0020
#define EXTRA_FLAG_LOADER 0x0040
#define EXTRA_FLAG_NOINHERIT 0x0080 /* for minherit(), if we implement it */
#define EXTRA_FLAG_SPECIAL 0x0100
#define EXTRA_FLAG_PRIMARY_STK                                                 \
    0x0200 /* Can indicate with MAP_STACK & EXTRA_FLAG_LOADER if we need a bit \
            */
#define EXTRA_FLAG_CACHE_CLEANED 0x0400 /* Only used with direct mappings */
#define EXTRA_FLAG_RLIMIT_DATA 0x1000   /* include in rlimit accounting */

struct mm_map {
    struct mm_map *next;
    struct mm_object_ref *obj_ref;
    off64_t offset;
    struct {
        struct mm_map *next;
        struct mm_map **owner;
    } ref;
    uintptr_t start;
    uintptr_t end;
    intptr_t reloc;
    unsigned mmap_flags;
    unsigned extra_flags;
    unsigned short last_page_bss;
    uint8_t spare;
    volatile uint8_t inuse;

    /* If things get added, there is code that needs to be modified... Grep for
     * "MAPFIELDS:" to find the places.
     */
};


/* struct mm_as (a.k.a. address_t) — kernel-half of a process as.
 *
 * Pre-PTE_U=0 this struct held both kernel state (cpu pgdir, asid) and a heap
 * of roottask-managed bookkeeping (map list, rlimit, anon, rwlock, tmap
 * scratch). All of that has migrated to root_process_t; only the kernel-touched
 * cpu state remains here. The struct stays kernel- allocated (PTE_U=0) because
 * the satp swap reads cpu.pgdir from S-mode.
 */
struct mm_as {
    struct cpu_mm_as cpu;
};


/* Sync object identification from physical address, using a single global
 * object (syspage). This will need a proper hash table later.
 */
#define PADDR_TO_SYNC_OBJ(addr) ((void *) _syspage_ptr)
#define PADDR_TO_SYNC_OFF(addr) ((paddr_t) (addr))

#ifndef CPU_ZERO_PAGE
#define CPU_ZERO_PAGE(d, l, mm) memset((d), 0, (l))
#endif

#ifndef CPU_FAULT_ON_WRITE_WORKS
#define CPU_FAULT_ON_WRITE_WORKS 1
#endif

#ifndef CPU_1TO1_FLUSH
#define CPU_1TO1_FLUSH(v, l)
#endif

extern unsigned cpu_as_vaddrinfo(process_t *, uintptr_t, paddr_t *, size_t *);


#define PTE_OP_MAP 0x0001
#define PTE_OP_PROT 0x0002
#define PTE_OP_UNMAP 0x0004
#define PTE_OP_PREALLOC 0x0008
#define PTE_OP_BAD 0x0010
#define PTE_OP_PREEMPT 0x0080
#define PTE_OP_TEMP 0x0200
#define PTE_OP_FORCEMERGE 0x0400

struct mm_pte_manipulate {
    paddr_t paddr;
    address_t *adp;
    uintptr_t start;
    uintptr_t end;
    uintptr_t split_end;
    uintptr_t first;
    unsigned prot;
    unsigned op;
    unsigned special;
    unsigned shmem_flags;
    unsigned tlb_flush_pending;
};

/* Kernel-side PTE ops. Live in arch/riscv/cpu-pte.c; called from
 * kernel/ext/manipulate.c. Declared here because the shared as.h is included by
 * both roottask and the kernel manipulate code.
 */
extern int cpu_pte_split(uintptr_t split, struct mm_pte_manipulate *);
extern int cpu_pte_manipulate(struct mm_pte_manipulate *);
extern int cpu_pte_merge(struct mm_pte_manipulate *);
extern void cpu_tlb_flush_complete(void);

int map_init(struct mm_map_head *, uintptr_t start, uintptr_t end);
int map_fini(struct mm_map_head *);
int map_create(struct map_set *,
               struct map_set *,
               struct mm_map_head *,
               uintptr_t va,
               uintptr_t size,
               uintptr_t mask,
               unsigned flags);
uintptr_t map_find_va(struct mm_map_head *,
                      uintptr_t va,
                      uintptr_t size,
                      uintptr_t mask,
                      unsigned flags);
int map_split(struct map_set *, size_t);
int map_add(struct map_set *);
int map_remove(struct map_set *);
int map_coalese(struct map_set *);
int map_destroy(struct map_set *);
void map_set_update(struct map_set *, struct mm_map *, struct mm_map *);

int rdecl rlimit_blown(pid_t pid, int which, size_t new);

void sysaddr_map(void *);

#define MI_NONE 0x00
#define MI_SPLIT 0x01
#define MI_NEXT 0x02
#define MI_SKIP_SPECIAL 0x04

int map_isolate(struct map_set *,
                struct mm_map_head *,
                uintptr_t start,
                size_t size,
                int flags);

extern int pte_map(address_t *,
                   uintptr_t,
                   uintptr_t,
                   int,
                   path_object_t *,
                   paddr_t,
                   unsigned);
extern int pte_prot(address_t *,
                    uintptr_t,
                    uintptr_t,
                    unsigned,
                    path_object_t *);
extern int pte_unmap(address_t *, uintptr_t, uintptr_t, path_object_t *);
extern int pte_bad(address_t *, uintptr_t);
extern int pte_prealloc(address_t *, uintptr_t, uintptr_t);
extern int pte_temp_map(address_t *,
                        uintptr_t,
                        struct pa_quantum *,
                        struct mm_map *,
                        size_t,
                        int (*func)(void *, size_t, void *),
                        void *);

extern int memref_add(struct mm_map *,
                      path_object_t *,
                      pid_t pid,
                      address_t *,
                      int fd);
extern void memref_del(struct mm_map *);
extern int memref_walk(path_object_t *,
                       int (*)(struct mm_object_ref *, struct mm_map *, void *),
                       void *);
extern void memref_walk_restart(struct mm_map_head *);

extern off64_t anmem_offset(path_object_t *, uintptr_t, size_t);
extern void anmem_unoffset(path_object_t *, off64_t, size_t);

extern int memobj_pmem_del_len(path_object_t *, off64_t, uint64_t);
extern int memobj_pmem_del_end(path_object_t *, off64_t, off64_t);
extern int memobj_pmem_add(path_object_t *, off64_t, size_t, int);
extern int memobj_pmem_add_pq(path_object_t *, off64_t, struct pa_quantum *);
extern int memobj_pmem_split(path_object_t *obp,
                             struct pa_quantum *pq,
                             unsigned);
extern int memobj_offset_check(path_object_t *, off64_t, uint64_t);

#define MPW_HOLES 0x01
#define MPW_PHYS 0x02
#define MPW_SYSRAM 0x04

extern int memobj_pmem_walk(
    int,
    path_object_t *,
    off64_t,
    off64_t,
    int (*func)(path_object_t *, off64_t, struct pa_quantum *, size_t, void *),
    void *);
extern int memobj_pmem_walk_mm(
    int,
    struct mm_map *,
    int (*func)(path_object_t *, off64_t, struct pa_quantum *, size_t, void *),
    void *);

/* These defines have to be different from all the user visible unmap flags */
#define UNMAP_PRIVATIZE 0x80000000u
#define UNMAP_NORLIMIT 0x40000000u

extern int ms_unmap(pid_t pid,
                    address_t *adp,
                    struct map_set *ms,
                    unsigned unmap_flags);
extern int mm_sync(path_object_t *, struct mm_map *, int);

#define MR_NONE 0x0000
#define MR_WRITE 0x0001
#define MR_NOINIT 0x0002
/* define unused     0x0004 */
#define MR_TRUNC 0x0008

extern int memory_reference(struct mm_map **,
                            uintptr_t,
                            uintptr_t,
                            unsigned,
                            struct map_set *);

#undef MMF
#define MMF(r, f, p, e) MMF_PROTO(r, f, p, e)
AS_FUNCS(as)
extern void as_init_mem(int phase);
extern int as_mcreate_pid(pid_t);
extern void as_mdestroy_pid(pid_t);
extern int munmap_flags_default;

/* map_spin (intrspin_t) was replaced with map_mutex (pthread_mutex_t) for
 * U-mode roottask: csrr/csrw sstatus is illegal in U-mode. pthread_mutex_t =
 * sync_t — use the underlying type to avoid pulling in <pthread.h> from this
 * internal header.
 */
extern sync_t map_mutex;
extern struct vaddr_range system_code_vaddr;
extern struct vaddr_range system_data_vaddr;

/* We can save some time by not locking the anonymous object sometimes if there
 * aren't multiple address spaces referencing it ("/proc/<pid>/as" mappings).
 * However, there's a gotcha. With the debug version of roottask, the
 * VERIFY_OBJ_LOCK() macros will start complaining that the object isn't locked.
 * So, if we're building a debug version, we pretend that the anon object is
 * always has multiple references, so it always gets locked and
 * VERIFY_OBJ_LOCK() is happy
 */
#ifdef NDEBUG
#define ANMEM_MULTI_REFS(obp) ((obp)->mem.mm.flags & MM_ANMEM_MULTI_REFS)
#else
#define ANMEM_MULTI_REFS(obp) 1
#endif

#endif /* _ROOTTASK_AS_H */
