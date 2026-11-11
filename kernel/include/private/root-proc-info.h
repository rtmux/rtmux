/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Shared struct layout for the __KSYS_PRIV_PROC_QUERY kernel extension: the
 * kernel fills this U-mode buffer from the kernel-half process_t fields the
 * U-mode roottask would otherwise dereference directly (now illegal with the
 * kernel heap mapped PTE_U=0).
 *
 * Kept separate from the rest of root-process.h because the kernel side
 * (kernel/ext/query.c) needs the same layout without dragging in the
 * roottask-only root_process_t type.
 */

#ifndef _SYS_ROOT_PROC_INFO_H
#define _SYS_ROOT_PROC_INFO_H

#include <sys/types.h>
#include <sys/siginfo.h>
#include <sys/rtmux-core.h> /* struct _cred_info */
#include <sys/resource.h>   /* RLIM_NLIMITS */
#include <cap-table.h>
#include <stdint.h>

typedef struct root_proc_info {
    pid_t pid;
    pid_t parent_pid;
    pid_t pgrp;
    uint32_t flags;
    uint32_t num_active_threads;
    uint32_t num_total_threads;
    uint8_t terming_priority;
    uint8_t process_priority;
    uint8_t pad0[2];
    /* Session summary (sess_leader == 0 if process has no session). */
    pid_t sess_leader;
    pid_t sess_pgrp;
    int32_t sess_fd;

    /* Raw session pointer. session_t is roottask-owned (libc malloc, see
     * process-session.c::_PROC_SESSION_SETSID), so the pointer's target is in
     * user-VA and U-mode roottask can dereference it directly.
     */
    void *session;
    /* Memory layout summary. */
    uintptr_t base_addr;
    uintptr_t boundary_addr;
    uint64_t max_cpu_time;

    /* CPU-time accounting (proc_wait reads these to fold a reaped child's
     * totals into the parent).
     */
    uint64_t running_time;
    uint64_t system_time;
    uint64_t kids_running_time;
    uint64_t kids_system_time;

    /* Memory presence flag — 1 if prp->memory. Used by the exit path to decide
     * whether to attempt munmap + process_shutdown. Surfaced via the
     * process_query_info snapshot rather than an inline "if (prp->memory)"
     * deref (the kernel heap is PTE_U=0).
     */
    bool has_memory;
    uint32_t pad2;

    /* prp->sys_chp — pointer to the system notification channel (proc service
     * or the process's own system listener). User-mode callers only test
     * whether it exists; the kernel owns delivery through the pointer.
     */
    void *sys_chp;

    /* prp->lcp — loader-context pointer (roottask-owned struct). The lcp itself
     * is in roottask user-VA (allocated by loader_load); we just need the
     * existence flag in the exit path's "did the child die during load?"
     * branch.
     */
    void *lcp;

    /* Per-process rlimit arrays — used by proc_resource handlers. Read-only
     * snapshot; writes go through process_set_rlimit (a TODO kernel extension
     * when an actual setrlimit call lands). Size matches
     * process_t.rlimit_vals_{soft,hard}.
     */
    unsigned long rlimit_vals_soft[RLIM_NLIMITS];
    unsigned long rlimit_vals_hard[RLIM_NLIMITS];

    /* prp->ocb_list — head of the procfs ocb list owned by this process.
     * roottask procfs.c reads it during teardown / unblock; writes go through
     * process_set_ocb_list (a kernel extension setter).
     */
    void *ocb_list;
    /* Snapshot of process_t.siginfo. */
    siginfo_t siginfo;
    /* Snapshot of credentials (process_t.cred->info). */
    struct _cred_info cred;
} root_proc_info_t;

/* Kernel PRIV kcall_args payload. Same format on both sides.
 *
 *   pid  (in)  — pid to query
 *   info (out) — U-mode VA pointer to the buffer to fill
 *   ret  (out) — EOK on success, ESRCH if the pid is gone
 */
struct kcall_args_proc_query {
    pid_t pid;
    root_proc_info_t *info;
    int ret;
};

struct kcall_args_proc_cap_stats {
    pid_t pid;
    rtmux_cap_stats_t *stats;
    int ret;
};

struct kcall_args_proc_endpoint_cap {
    pid_t pid;
    uint32_t coid;
    rtmux_cap_t cap;
    uint32_t rights;
    int ret;
};

/* Mint a RTMUX_CAP_TYPE_PROCESS capability over the target process into the
 * caller's own endpoint_caps table. rights must be a nonzero subset of
 * RTMUX_CAP_PROCESS_ALL. On success cap holds the minted handle.
 */
struct kcall_args_proc_cap_mint {
    pid_t pid;
    uint32_t rights;
    rtmux_cap_t cap;
    int ret;
};

#endif /* _SYS_ROOT_PROC_INFO_H */
