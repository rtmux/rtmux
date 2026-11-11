/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Roottask-side privileged-call wrappers.
 *
 * Each wrapper packs its arguments into a kernel-call payload and enters the
 * kernel via syscall_priv(kext_*, &args). syscall_priv is the privileged-
 * callback escape: from U-mode roottask the libc weak stub at
 * lib/libc/riscv/syscalls.c traps via ECALL into ksys_call_table[__KSYS_PRIV]
 * -> ksys_priv(act, kap), which checks priv_caps and runs the kext_* body. A
 * caller without the matching cap cannot invoke these handlers, even by
 * guessing the sub-ID.
 *
 * The kext_* bodies live in kernel/ext/{bind,reparent,exit, process}.c; their
 * addresses are resolved against the kernel symbol table by the ELF loader at
 * roottask-load time.
 */

#include "externs.h"
#include "as.h"
#include "root-process.h"

/* Body declarations — defined under kernel/ext/, addresses resolved against the
 * kernel symbol table.
 */

/* process_bind — bind the calling roottask thread to a target process's address
 * space (pid 0 unbinds). Switches the active satp so roottask code can run with
 * the target process's page tables. Parameters:
 *   pid : target process pid, or 0 to unbind
 * Return value: EOK on success, errno on failure.
 */
int process_bind(pid_t pid)
{
    return (int) (syscall_priv(__KSYS_PRIV_PROCESS_BIND, &pid));
}

struct reparent_args {
    process_t *prp;
};

/* process_reparent — reparent a process to pid 1 ("stay resident"). Parameters:
 *   prp : opaque kernel-VA pointer to the process being reparented
 */
void process_reparent(process_t *prp)
{
    struct reparent_args args = {.prp = prp};
    syscall_priv(__KSYS_PRIV_REPARENT, &args);
}

struct kext_destroy_args {
    process_t *prp;
    int id;
    int result;
};

/* process_threads_destroy — destroy all threads of a process. Parameters:
 *   prp : opaque kernel-VA pointer to the target process
 */
void process_threads_destroy(process_t *prp)
{
    syscall_priv(__KSYS_PRIV_EXIT_DESTROY_THREADS, prp);
}

/* process_timer_destroy — destroy one timer of a process. Parameters:
 *   prp : opaque kernel-VA pointer to the target process
 *   id  : timer id to destroy
 * Return value: kernel result of the timer-destroy operation.
 */
int process_timer_destroy(process_t *prp, int id)
{
    struct kext_destroy_args args = {.prp = prp, .id = id};
    syscall_priv(__KSYS_PRIV_TIMER_DESTROY, &args);
    return args.result;
}

/* process_connect_detach — detach one connection of a process. Parameters:
 *   prp  : opaque kernel-VA pointer to the target process
 *   coid : connection id to detach
 * Return value: kernel result of the connection-detach operation.
 */
int process_connect_detach(process_t *prp, int coid)
{
    struct kext_destroy_args args = {.prp = prp, .id = coid};
    syscall_priv(__KSYS_PRIV_CONNECT_DETACH, &args);
    return args.result;
}

struct kcall_args_process_create {
    pid_t parent_pid;
    void *lcp;

    /* Mirrors kernel/ext/process.c::kcall_args_process_create. The kernel
     * writes adp_out and pid_out before SETKSTATUS so the roottask side never
     * has to deref the freshly-created kernel-half process_t.
     */
    struct mm_as *adp_out;
    pid_t pid_out;
};

/* process_create — create a new process under a given parent.
 *
 * The kernel half (kext_process_create) allocates the process_t, sets up its
 * address space, and returns the new pid + as pointer via the kcall_args struct
 * so roottask never dereferences the freshly-created kernel-half process_t.
 * This wrapper then claims the roottask-side per-process slot, inherits parent
 * state (umask), and finishes the roottask-half memory setup via
 * as_mcreate_pid.
 *
 * Parameters:
 *   parent_pid : pid of the creating process
 *   lcp        : loader context to associate with the new process
 *   out_pid    : if non-NULL, receives the new process's pid (0 on error)
 * Return value: kernel-VA process_t pointer, or (process_t *)-1 on failure.
 */
process_t *process_create(pid_t parent_pid, void *lcp, pid_t *out_pid)
{
    struct kcall_args_process_create data;
    process_t *prp;
    pid_t new_pid;

    data.parent_pid = parent_pid;
    data.lcp = lcp;
    data.adp_out = NULL;
    data.pid_out = 0;

    prp = (process_t *) syscall_priv(__KSYS_PRIV_PROCESS_CREATE, &data);
    if (prp == (void *) -1 || !prp) {
        if (out_pid)
            *out_pid = 0;
        return prp;
    }

    /* Claim the roottask-side per-process slot before any code path that writes
     * root_proc(pid)->X. pid + adp come back via the kcall_args struct (kernel
     * writes them before SETKSTATUS), so we never deref the kernel-allocated
     * process_t here.
     */
    new_pid = data.pid_out;
    root_process_init(new_pid, parent_pid);
    root_proc(new_pid)->adp = data.adp_out;

    /* Inherit roottask-side per-process state from the parent. These used to be
     * inherited inside kext_process_create (umask=parent-> umask) but now live
     * in root_process_t, so the kernel can no longer see them. Inheritance is
     * roottask bookkeeping — done here.
     */
    if (parent_pid != 0) {
        root_process_t *parent_tmp = root_proc(parent_pid);
        root_process_t *child_tmp = root_proc(new_pid);
        child_tmp->umask = parent_tmp->umask;

        /* POSIX: a child starts in its parent's process group and session. A
         * parent that never joined a group is its own group (pgrp 0 is unset),
         * so its children are in group parent_pid. Each member holds one link on
         * the session; root_process_fini drops it.
         */
        child_tmp->pgrp = parent_tmp->pgrp ? parent_tmp->pgrp : parent_pid;
        if (parent_tmp->session) {
            child_tmp->session = parent_tmp->session;
            (void) atomic_add_value(&child_tmp->session->links, 1);
        }
    }

    /* Kernel mm_mcreate has set up adp + cpu pgdir. Fill in the roottask-half
     * (mm_map_head, anon backing object, rlimits) in root_process_t — as_mmap
     * needs root_proc(pid)->map.{start,end} populated before any user mmap can
     * succeed.
     */
    if (as_mcreate_pid(new_pid) != EOK) {
        /* Roll back via the regular destroy path. */
        (void) process_destroy_all(new_pid);
        (void) process_destroy_call(new_pid);
        root_process_fini(new_pid);
        if (out_pid)
            *out_pid = 0;
        return (process_t *) -1;
    }
    if (out_pid)
        *out_pid = new_pid;
    return prp;
}

/* process_destroy_all — tear down all of a process's kernel objects.
 * Parameters:
 *   pid : target process pid
 * Return value: EOK on success, errno on failure.
 */
int process_destroy_all(pid_t pid)
{
    return (int) (syscall_priv(__KSYS_PRIV_PROCESS_DESTROYALL,
                               (void *) (uintptr_t) pid));
}

/* process_destroy_call — destroy the kernel-half process_t and release the
 * roottask-side per-process slot so it can be reused by a future generation.
 * Parameters:
 *   pid : target process pid
 * Return value: EOK on success, errno on failure.
 */
int process_destroy_call(pid_t pid)
{
    int rc;

    /* The kernel refuses (EAGAIN) while a roottask thread is inside a
     * query_object() on this process; that window is short, so yield and retry.
     * Releasing the roottask slot before the kernel half is really gone would
     * strand a live process (see root_proc_lock).
     */
    for (unsigned tries = 0;; tries++) {
        rc = (int) (syscall_priv(__KSYS_PRIV_PROCESS_DESTROY,
                                 (void *) (uintptr_t) pid));
        if ((rc != EAGAIN && rc != -EAGAIN) || tries >= 100000)
            break;
        (void) __sched_yield();
    }

    /* Retries exhausted: the kernel half is still alive, so keep the slot. */
    if (rc == EAGAIN || rc == -EAGAIN)
        return rc;

    /* Release the roottask-side per-process slot once the kernel half is gone.
     * proc_exit has already nulled root/cwd/etc; this just clears the pid tag
     * so the slot is reusable for a future generation.
     */
    root_process_fini(pid);
    return rc;
}

struct kcall_args_process_startup {
    pid_t pid;
    int start;
    uint64_t priv_caps;
};

/* process_startup: fetch (and optionally start) a process's loader context
 * during the spawn handshake. Parameters:
 *   pid       : target process pid
 *   start     : non-zero to actually start the process running
 *   priv_caps : capability set to commit onto the child at start; ignored when
 *               start is zero. Must be a subset of the caller's own caps or the
 *               kernel rejects the start with EPERM.
 * Return value: the process's loader-context pointer, or an encoded
 *   error (NULL / value in [-256, -1]) on failure.
 */
void *process_startup(pid_t pid, int start, uint64_t priv_caps)
{
    struct kcall_args_process_startup data;

    data.pid = pid;
    data.start = start;
    data.priv_caps = priv_caps;
    return (void *) syscall_priv(__KSYS_PRIV_PROCESS_STARTUP, &data);
}

struct kcall_args_process_exec {
    void *lcp;
    int *prcvid;
    int rcvid;
    pid_t ppid;
};

/* process_exec — perform the exec() image-replacement step. Parameters:
 *   lcp    : loader context for the replacement image
 *   prcvid : in/out pointer to the rcvid stored in the loader context
 *   rcvid  : rcvid of the message driving the exec
 *   ppid   : parent pid
 * Return value: EOK on success, -1 on failure (errno set).
 */
int process_exec(void *lcp, int *prcvid, int rcvid, int ppid)
{
    struct kcall_args_process_exec data;

    data.lcp = lcp;
    data.prcvid = prcvid;
    data.rcvid = rcvid;
    data.ppid = ppid;
    return (int) (syscall_priv(__KSYS_PRIV_PROCESS_EXEC, &data));
}

/* process_shutdown — release a process's address space.
 *
 * The body (kext_process_shutdown) handles the IPI_TLB_SAFE round-trip inline:
 * it sends the IPI to every remote hart still pointing at the dying as, waits
 * for ipicmds[] to clear, drains in-flight message delivery, then proceeds to
 * mdestroy. The wrapper is therefore the usual syscall_priv + errno-conversion
 * shape, with no per-CPU spin in U-mode that would need ipicmds[] /
 * get_active_thread() (kernel-only).
 *
 * Parameters:
 *   prp  : opaque kernel-VA pointer to the target process
 *   exec : non-zero if shutting down for an exec() image replacement
 * Return value: EOK on success, -1 on failure (errno set; EAGAIN means
 *   a remote hart still holds the as — retry).
 */
struct kcall_args_process_shutdown {
    process_t *prp;
    int exec;
};

int process_shutdown(process_t *prp, int exec)
{
    struct kcall_args_process_shutdown data;
    int r;

    data.prp = prp;
    data.exec = exec;
    r = (int) syscall_priv(__KSYS_PRIV_PROCESS_SHUTDOWN, &data);

    /* kcall_err() sets SETKSTATUS(thp, -err), so syscall_priv returns -errno on
     * failure. Convert to the -1 + errno convention callers expect.
     */
    if (r < 0) {
        errno = -r;
        r = -1;
    }
    return r;
}
