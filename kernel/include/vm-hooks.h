/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Virtual memory hooks */

#ifndef _VM_HOOKS_H
#define _VM_HOOKS_H

/* Forward declaration: VM hooks use path_object_t only as a pointer. */
union path_object;
typedef union path_object path_object_t;

struct _procfs_map_info;
struct _procfs_debug_info;
struct _mem_debug_info;
struct fault_info;

/* Signature note on "pid_t pid" first arg:
 *
 * After the heap-PTE_U=0 / U-mode-roottask flip, the roottask half of this
 * table (struct vm_ops_t "vm_ops") cannot dereference the kernel- allocated
 * process_t pointer — heap is PTE_U=0, U-mode loads fault. For functions called
 * from roottask context, pid is the canonical handle: root_proc(pid) gets you
 * the roottask-half state (map list, rlimit, anon, …), and root_proc(pid)->adp
 * gets you the kernel-half pointer to pass into pte_* kernel extensions.
 *
 * The kernel half (vm_ops, kernel/mm/core.c) ignores pid where it still has prp
 * in scope. The two halves share the same MMF table shape.
 */
#define MM_FUNCS(extra)                                                       \
    MMF(void, init_mem, (int phase), extra)                                   \
    MMF(void, as, (process_t * prp, process_t * *pactprp), extra)             \
    MMF(int, fault, (struct fault_info *), extra)                             \
    MMF(int, map_xfer,                                                        \
        (process_t * actprp, process_t * prp, iov_t * *piov, int *pparts,     \
         int *poff, iov_t *niov, int *pnparts, unsigned flags),               \
        extra)                                                                \
    MMF(int, mcreate, (process_t * prp), extra)                               \
    MMF(void, mdestroy, (process_t * prp), extra)                             \
    MMF(int, dup,                                                             \
        (pid_t pprp_pid, process_t * pprp, pid_t prp_pid, process_t * prp),   \
        extra)                                                                \
    MMF(int, mmap,                                                            \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int prot,    \
         int flags, path_object_t *obp, uint64_t off, size_t align,           \
         size_t prealloc, int fd, void **vaddr, size_t *size),                \
        extra)                                                                \
    MMF(int, munmap,                                                          \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int flags),  \
        extra)                                                                \
    MMF(int, mprotect,                                                        \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int prot),   \
        extra)                                                                \
    MMF(int, msync,                                                           \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int flags),  \
        extra)                                                                \
    MMF(unsigned, vaddrinfo,                                                  \
        (pid_t pid, process_t * prp, uintptr_t vaddr, paddr_t * paddrp,       \
         size_t * lenp, unsigned flags),                                      \
        extra)                                                                \
    MMF(path_object_t *, vaddr_to_memobj,                                     \
        (process_t * prp, void *vaddr, paddr_t *offset, int create), extra)   \
    MMF(size_t, mapinfo,                                                      \
        (pid_t pid, process_t * prp, uintptr_t vaddr,                         \
         struct _procfs_map_info * info, struct _procfs_debug_info * debug,   \
         size_t, path_object_t **, int *fd, size_t *contigp),                 \
        extra)                                                                \
    MMF(int, debuginfo,                                                       \
        (pid_t pid, process_t * prp, struct _mem_debug_info * info), extra)   \
    MMF(int, resize, (path_object_t * obp, size_t size), extra)               \
    MMF(int, memobj_phys, (path_object_t * obp, paddr_t pa), extra)           \
    MMF(int, validate,                                                        \
        (pid_t pid, process_t * prp, uintptr_t start, size_t len, int flags), \
        extra)                                                                \
    MMF(int, madvise,                                                         \
        (pid_t pid, process_t * prp, uintptr_t start, size_t len, int flags), \
        extra)

#define AS_FUNCS(extra)                                                       \
    MMF(void, init_mem, (int phase), extra)                                   \
    MMF(int, dup,                                                             \
        (pid_t pprp_pid, process_t * pprp, pid_t prp_pid, process_t * prp),   \
        extra)                                                                \
    MMF(int, mmap,                                                            \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int prot,    \
         int flags, path_object_t *obp, uint64_t off, size_t align,           \
         size_t prealloc, int fd, void **vaddr, size_t *size),                \
        extra)                                                                \
    MMF(int, munmap,                                                          \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int flags),  \
        extra)                                                                \
    MMF(int, mprotect,                                                        \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int prot),   \
        extra)                                                                \
    MMF(int, msync,                                                           \
        (pid_t pid, process_t * prp, uintptr_t addr, size_t len, int flags),  \
        extra)                                                                \
    MMF(unsigned, vaddrinfo,                                                  \
        (pid_t pid, process_t * prp, uintptr_t vaddr, paddr_t * paddrp,       \
         size_t * lenp, unsigned flags),                                      \
        extra)                                                                \
    MMF(path_object_t *, vaddr_to_memobj,                                     \
        (process_t * prp, void *vaddr, paddr_t *offset, int create), extra)   \
    MMF(size_t, mapinfo,                                                      \
        (pid_t pid, process_t * prp, uintptr_t vaddr,                         \
         struct _procfs_map_info * info, struct _procfs_debug_info * debug,   \
         size_t, path_object_t **, int *fd, size_t *contigp),                 \
        extra)                                                                \
    MMF(int, debuginfo,                                                       \
        (pid_t pid, process_t * prp, struct _mem_debug_info * info), extra)   \
    MMF(int, resize, (path_object_t * obp, size_t size), extra)               \
    MMF(int, memobj_phys, (path_object_t * obp, paddr_t pa), extra)           \
    MMF(int, validate,                                                        \
        (pid_t pid, process_t * prp, uintptr_t start, size_t len, int flags), \
        extra)                                                                \
    MMF(int, madvise,                                                         \
        (pid_t pid, process_t * prp, uintptr_t start, size_t len, int flags), \
        extra)

typedef struct vm_ops_entry {
    unsigned pagesize;
    unsigned sizeof_address_entry;

#undef MMF
#define MMF(r, f, p, e) r(*f) p;
    MM_FUNCS(x)
    void *reserved[2];
} vm_ops_t;

#define MMF_PROTO(r, f, p, e) extern r e##_##f p;
#define MMF_DEFN(r, f, p, e) e##_##f,

/* Process lifecycle callbacks */
typedef struct proc_hooks_entry {
    void (*proc_threads_destroyed)(process_t *prp, struct sigevent *ev);
    int proc_stack_code;
} proc_hooks_t;

/* Definitions for parameters */

/* flags for map_addr */
#define MAPADDR_FLAGS_WRITE 0x1
#define MAPADDR_FLAGS_SYSPRP 0x2
#define MAPADDR_FLAGS_NOTFIRST 0x4
#define MAPADDR_FLAGS_IOVKERNEL 0x8000

#endif
