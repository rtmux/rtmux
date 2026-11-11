/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _ROOTTASK_ROOT_PROCESS_H
#define _ROOTTASK_ROOT_PROCESS_H

#include <sys/types.h>
#include <sys/siginfo.h>
#include <private/root-proc-info.h>
#include <stdbool.h>
#include <stdint.h>
#include <cap-table.h>

/* Forward decls. node_t and vector_t are defined in kernel/include/objects.h
 * (roottask pulls that header in for the kernel-half process_t shape). struct
 * procfs_waiting / struct wait_entry are roottask-internal; declared in
 * path/procfs_priv.h and include/process.h respectively.
 */
struct node_entry;   /* node_t */
struct vector_entry; /* vector_t */
struct wait_entry;
struct procfs_waiting;
struct proc_mux_lock;       /* servers/roottask/sys/support.c — opaque */
struct conf_entry;          /* servers/roottask/sys/sysconf.c */
struct hwres_process_state; /* servers/roottask/sys/hwres-*.c */

/* Forward decls for the mm fields we hold per-process. Definitions are in
 * servers/roottask/include/as.h and path-object.h.
 */
struct mm_as;
struct mm_map;
union path_object;
typedef union path_object path_object_t;

#include "mm-map-head.h"

/* root_process_t
 *
 * Roottask-side per-process state. Fields here used to live in
 * kernel/include/objects.h's "process_t", parked there back when roottask
 * shared the kernel address space. None of these fields are read or written by
 * the kernel; they are entirely roottask-internal state about the process.
 *
 * With heap PTE_U=0 (see feedback_heap_pte_u_zero.md), U-mode roottask cannot
 * dereference a kernel-allocated process_t at all. Moving these fields into a
 * roottask-owned, roottask-VA struct removes one whole class of cross-boundary
 * deref.
 *
 * Storage: a fixed-size array "root_process[]" of ROOT_PROCESS_MAX entries in
 * roottask's BSS, indexed by PINDEX(pid) = (pid & 0xfff). This matches the
 * kernel's PID_MASK in macros.h: every live process has a unique slot index,
 * and slots are reused across pid generations.
 *
 * The "pid" field of each slot is a sanity tag — it stores the full pid (gen |
 * index) of whichever process currently occupies the slot, or 0 when the slot
 * is free. Callers should verify before trusting slot contents on a stale-pid
 * path.
 */
typedef struct root_process_entry {
    pid_t pid;                            /* full pid; 0 = slot free */
    pid_t parent_pid;                     /* mirror of parent->pid; needed
                                           * by root_proc_lock_parent without
                                           * dereferencing the kernel-half
                                           * process_t.parent pointer
                                           */
    struct proc_mux_lock *lock;           /* roottask-side process mux —
                                           * lazily allocated by acquire_lock
                                           * (see servers/roottask/sys/support.c).
                                           * Was process_t.lock; moved here
                                           * because the lock state is purely
                                           * roottask-managed and the kernel
                                           * never observes it.
                                           */
    struct node_entry *root;              /* roottask namespace root dir */
    struct node_entry *cwd;               /* roottask namespace current dir */
    struct vector_entry *events;          /* proc_event vector */
    struct wait_entry *wap;               /* proc_wait list head */
    struct procfs_waiting *debug_waiting; /* procfs debug-waiter list */
    struct conf_entry *conf_table;        /* sysconf/confstr table head */
    struct hwres_process_state *hwres_state; /* hwres per-process state */
    uint32_t umask; /* file-creation mask — roottask-only */
    /* DMA/MMIO containment: once a process self-restricts (via
     * _PROC_PHYS_CONTAIN) this seals its address space — every further
     * specific-address MAP_PHYS is refused (MAP_ANON DMA buffers are
     * unaffected). Irreversible; roottask-only. Default false => no
     * containment, historic behavior.
     */
    bool phys_contained;

    /* Set at spawn when the image is listed in the bootstrap capability
     * manifest (a system server). kill(-1) skips such processes.
     */
    bool system;

    /* Address-space rwlock state, used to serialize roottask-side ops on a
     * process's address_t (mmap / munmap / fault / etc.). Used to live in
     * adp->rwlock and adp->fault_owner, but the kernel never observed them —
     * purely roottask-managed coordination. Moved here so roottask can
     * read/write them without dereferencing the kernel-allocated address_t.
     */
    unsigned rwlock_lock;
    unsigned long fault_owner;

    /* Address-space content (the user-VA mm_map list, anon backing object,
     * rlimit accounting, temp-map scratch). These fields used to live in struct
     * mm_as (kernel-allocated address_t), where the kernel never read them —
     * see kernel/mm/mcreate.c:24-27, "roottask tracks ranges for user mmap; the
     * kernel has no business in that bookkeeping". With heap PTE_U=0, U-mode
     * roottask cannot deref the kernel-allocated adp at all, so the fields move
     * here. struct mm_as shrinks to just "cpu" (kernel-side pgdir/asid).
     */
    struct mm_map_head map;
    struct {
        uintptr_t vmem;
        uintptr_t data;
        uintptr_t stack;
        uintptr_t memlock;
        uintptr_t rss;
    } rlimit;
    path_object_t *anon;
    rtmux_cap_table_t mem_caps;
    rtmux_cap_slot_t *mem_cap_slots;
    uintptr_t tmap_base;
    size_t tmap_size;

    /* Cached pointer to the kernel-allocated address_t for this process. The
     * kernel sets this via __KSYS_PRIV_PROCESS_GET_ADP after mcreate; U-mode
     * roottask never DEREFERENCES adp (heap is PTE_U=0) — it just passes the
     * pointer value to kernel extensions (pte_map, pte_prot, …) that need it
     * for adp->cpu.pgdir on the kernel side.
     */
    struct mm_as *adp;

    /* POSIX session / process-group / fault siginfo.
     *
     * These are POSIX policy state — a microkernel has no business looking at
     * them. They lived in process_t back when the proc service and kernel
     * shared an address space and "connect_client_info" could cheaply bundle
     * the session id into its reply; with U-mode roottask that's no longer
     * free, and the RTMux ABI for "_client_info" has dropped "sid" /
     * "BKGND_PGRP" (see connect.c).
     *
     *   pgrp          — POSIX process-group id; default = pid.
     *   session       — roottask-allocated session_t (libc malloc'd).
     *                   Shared between processes via session->links.
     *   siginfo       — fault / exit info reported via wait().  Filled
     *                   on faults (deliver_fault) and exits (proc_exit).
     */
    pid_t pgrp;
    struct session_entry *session;
    siginfo_t siginfo;

    /* Job control: which of SIGTTIN/SIGTTOU the process blocks or ignores
     * (_JOBCTL_HOLD_* bits), as its libc reports through _PROC_JOBCTL_STATE. A
     * terminal server reads it to choose between EIO and a stop signal.
     */
    uint32_t tty_hold;

    /* Executable path saved at _PROC_SPAWN_DEBUG time. Read by:
     *   - servers/roottask/path/procfs.c (DEVCTL_PROC_MAPDEBUG_BASE,
     * /proc/PID/name)
     *   - servers/roottask/mem/as/mapinfo.c (mapdebug path stamp)
     * Stored roottask-side because the path is POSIX namespace state and /proc
     * name readers have to reach it from U-mode. Used to live as
     * "prp->debug_name" in the kernel heap (PTE_U=0 made it unreadable from
     * U-mode roottask); the kernel-side diagnostic readers in shutdown.c /
     * event.c / stall-detector.c gracefully degrade to "?" without it.
     *
     * Lifecycle: strdup'd at spawn time, free'd in root_process_fini.
     */
    char *debug_name;
} root_process_t;

/* 4096 = PID_MASK+1, matching kernel/core/macros.h's PINDEX(). Bumping this
 * requires a coordinated bump of PID_MASK on the kernel side.
 */
#define ROOT_PROCESS_MAX 4096
#define ROOT_PINDEX(pid) ((unsigned) (pid) & 0xfff)

extern root_process_t root_process[ROOT_PROCESS_MAX];

/* Resolve a pid to its root_process_t slot.
 *
 * Returns the slot pointer unconditionally; the slot may belong to a different
 * generation if the caller is racing process death.
 */
static inline root_process_t *root_proc(pid_t pid)
{
    return &root_process[ROOT_PINDEX(pid)];
}

void root_process_init(pid_t pid, pid_t parent_pid); /* claim slot at create */
void root_process_fini(pid_t pid); /* release slot at destroy */
void memcap_process_fini(root_process_t *tmp);

/* root_proc_info_t — see kernel/include/private/root-proc-info.h for the shared
 * struct layout. Populated by process_query_info (a PRIV kernel extension that
 * copies the requested fields out kernel-side).
 *
 * Not every process_t field is mirrored — only the ones roottask code paths
 * actually inspect. When a new field is needed, add it to the struct and to
 * kext_proc_query() in kernel/ext/query.c; never expose the raw kernel pointer.
 *
 * The snapshot is consistent at the moment of the kernel extension call.
 * Callers holding root_proc_lock(pid) get the snapshot they expect; callers
 * that don't lock may race with concurrent kernel updates to flags, threads,
 * etc.
 *
 * Returns EOK on success or ESRCH if the pid no longer maps to a live
 * process_t.
 */
int process_query_info(pid_t pid, root_proc_info_t *info);

/* Kernel object-pool gauges for /sys/kstats; see
 * kernel/include/private/kstats.h.
 */
struct kstat_soul_rec;
int soul_stats(struct kstat_soul_rec *out,
               unsigned n,
               unsigned *count_out,
               unsigned *live_threads,
               unsigned *live_timeouts,
               uint64_t *timer_intr,
               unsigned timer_n,
               unsigned *timer_count);

#endif /* _ROOTTASK_ROOT_PROCESS_H */
