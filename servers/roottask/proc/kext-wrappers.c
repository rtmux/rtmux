/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Group B: thin wrappers around the roottask privileged-call sub-namespace
 * (\texttt{__KSYS_PRIV_*}). Each wrapper packs its arguments into a
 * kcall_args_* struct and calls syscall_priv(__KSYS_PRIV_X, &args). The
 * corresponding kext_* body lives in the kernel; reachable only through the
 * dispatch table in kernel/core/priv.c, gated by priv_caps.
 *
 * See Chapter 2 of the SysArch document for the architectural shape this file
 * implements.
 *
 * The kcall_args_* struct definitions are duplicated between this file and the
 * corresponding source file under kernel/ext/. Layout MUST match; if a struct
 * is changed on one side, the other side must be updated in lockstep.
 */

#include "externs.h"
#include <private/root-proc-info.h>
#include <private/kstats.h>
#include <sys/debug.h> /* enum rtmux_debug_request */

/* Cred (cred.c) */
struct kcall_args_cred_get {
    pid_t pid;
    struct _cred_info *cip;
};

struct kcall_args_cred_set {
    pid_t pid;
    struct _cred_info *cip;
};

/* cred_get_call — fetch a process's credentials into a caller buffer.
 * Parameters:
 *   pid : target process pid
 *   cip : buffer that receives the credential info
 * Return value: EOK on success, errno on failure.
 */
int cred_get_call(pid_t pid, struct _cred_info *cip)
{
    struct kcall_args_cred_get data;
    data.pid = pid;
    data.cip = cip;
    return (int) syscall_priv(__KSYS_PRIV_CRED_GET, &data);
}

/* cred_set_call — install new credentials on a process. Parameters:
 *   pid : target process pid
 *   cip : credential info to install
 * Return value: EOK on success, errno on failure.
 */
int cred_set_call(pid_t pid, struct _cred_info *cip)
{
    struct kcall_args_cred_set data;
    data.pid = pid;
    data.cip = cip;
    return (int) syscall_priv(__KSYS_PRIV_CRED_SET, &data);
}

/* Debug (debug.c + thread-debug.c for debug_install) */
struct kcall_args_debug_attach {
    pid_t pid;
    unsigned flags;
};

struct kcall_args_debug_detach {
    pid_t pid;
};

struct kcall_args_debug_process {
    pid_t pid;
    int tid;
    union rtmux_debug_data *data;
    enum rtmux_debug_request request;
};

/* kext_thread_ctl_call packs (prp, tid, cmd, data) — layout MUST match the
 * kernel-side definition (kernel/core/args.h's kcall_args_thread_ctl is
 * private; we mirror its (cmd, data) tail here).
 */
struct kcall_args_tctl_args {
    process_t *prp;
    int tid;
    int32_t cmd;
    void *data;
};

/* debug_attach — attach a debugger to a process. Parameters:
 *   pid   : target process pid
 *   flags : debug-attach flags
 * Return value: EOK on success, errno on failure.
 */
int debug_attach(pid_t pid, unsigned flags)
{
    struct kcall_args_debug_attach args;
    args.pid = pid;
    args.flags = flags;
    return (int) syscall_priv(__KSYS_PRIV_DEBUG_ATTACH, &args);
}

/* debug_detach — detach the debugger from a process. Parameters:
 *   pid : target process pid
 * Return value: EOK on success, errno on failure.
 */
int debug_detach(pid_t pid)
{
    struct kcall_args_debug_detach args;
    args.pid = pid;
    return (int) syscall_priv(__KSYS_PRIV_DEBUG_DETACH, &args);
}

/* debug_install — install the debug exception vector. Parameters:
 *   code : debug-install code passed through to the kernel
 */
void debug_install(int code)
{
    syscall_priv(__KSYS_PRIV_DEBUG_INSTALL, (void *) (intptr_t) code);
}

/* debug_process_call — issue a debug request against a process/thread.
 * Parameters:
 *   request : debug request selector
 *   pid     : target process pid
 *   tid     : target thread id
 *   data    : in/out debug-data payload for the request
 * Return value: request result on success, -1 on failure (errno set).
 */
int debug_process_call(enum rtmux_debug_request request,
                       pid_t pid,
                       int tid,
                       union rtmux_debug_data *data)
{
    struct kcall_args_debug_process args;
    int r;
    args.request = request;
    args.pid = pid;
    args.tid = tid;
    args.data = data;
    r = (int) syscall_priv(__KSYS_PRIV_DEBUG_PROCESS, &args);
    if (r < 0) {
        errno = -r;
        r = -1;
    }
    return r;
}

/* kext_thread_ctl_call — perform a thread-control operation on a thread.
 * Parameters:
 *   prp  : opaque kernel-VA pointer to the owning process
 *   tid  : target thread id
 *   cmd  : RTMUX_TCTL_* command selector
 *   data : command-specific payload (RUNMASK is copied by value)
 * Return value: kernel result of the thread-control operation.
 */
int kext_thread_ctl_call(process_t *prp, int tid, int32_t cmd, void *data)
{
    struct kcall_args_tctl_args args;
    args.prp = prp;
    args.tid = tid;
    args.cmd = cmd;
    args.data = data;
    /* Sigh - adjust for RUNMASK */
    if (cmd == RTMUX_TCTL_RUNMASK)
        memcpy(&args.data, data, sizeof(args.data));
    return (int) syscall_priv(__KSYS_PRIV_THREAD_CTL, &args);
}

/* U-mode roottask never takes the kernel private-call context lock. */

/* memobj_destroyed (core.c) */
struct kcall_args_synchash_rem {
    paddr_t addr;
    path_object_t *obp;
    paddr_t start;
    paddr_t end;
    process_t *prp;
    void *vaddr;
};

#define HASH_PAGESIZE 4096 /* same as kernel; sync_hash bucket page size */

/* memobj_destroyed — remove sync_hash entries for a destroyed memory object,
 * page by page over the [start, end] physical range. Parameters:
 *   obp   : namespace object the memory belonged to
 *   start : first physical address of the destroyed region
 *   end   : last physical address of the destroyed region
 *   prp   : opaque kernel-VA pointer to the owning process
 *   vaddr : virtual address the region was mapped at
 */
void memobj_destroyed(path_object_t *obp,
                      paddr_t start,
                      paddr_t end,
                      process_t *prp,
                      void *vaddr)
{
    struct kcall_args_synchash_rem data;
    long ret;

    data.addr = start;
    data.obp = obp;
    data.start = start;
    data.end = end;
    data.prp = prp;
    data.vaddr = vaddr;

    if (data.addr > (data.end | (HASH_PAGESIZE - 1)))
        return;
    for (;;) {
        ret = syscall_priv(__KSYS_PRIV_SYNCHASH_REM, &data);
        if ((data.addr & ~(paddr_t) (HASH_PAGESIZE - 1)) ==
            (data.end & ~(paddr_t) (HASH_PAGESIZE - 1)))
            break;
        if (ret != 0) {
            /* synchash_rem faulted during the writability check; skip this
             * bucket and advance to avoid an infinite loop
             */
            data.addr += HASH_PAGESIZE;
        }
    }
}

/* Query (query.c) */
struct kcall_args_query_object {
    unsigned type;
    unsigned index1;
    int subtype;
    unsigned index2;
    unsigned *next;
    void *objbuf;
    size_t objsize;
    unsigned restarts;
};

/* query_object — enumerate a kernel object (process, thread, channel, etc.) and
 * optionally copy a snapshot into a caller buffer. Parameters:
 *   type    : object class to query
 *   index1  : primary index (e.g. pid)
 *   subtype : object sub-selector
 *   index2  : secondary index
 *   next    : in/out iteration cursor advanced past the returned object
 *   objbuf  : optional buffer that receives an object snapshot
 *   objsize : size of objbuf, in bytes
 * Return value: the kernel-VA object pointer, or NULL when iteration
 *   is exhausted.
 */
void *query_object(unsigned type,
                   unsigned index1,
                   int subtype,
                   unsigned index2,
                   unsigned *next,
                   void *objbuf,
                   size_t objsize)
{
    struct kcall_args_query_object data;
    data.type = type;
    data.index1 = index1;
    data.subtype = subtype;
    data.index2 = index2;
    data.next = next;
    data.objbuf = objbuf;
    data.objsize = objsize;
    data.restarts = 0;
    return (void *) syscall_priv(__KSYS_PRIV_QUERY_OBJECT, &data);
}

/* query_object_done — clear process_t.querying for "pid" so a pending
 * process_destroy_call() can proceed. Wraps __KSYS_PRIV_QUERY_OBJECT_DONE.
 * Parameters:
 *   pid : process whose querying flag is cleared
 */
void query_object_done(pid_t pid)
{
    /* Whoever called query_object on a process is done looking at the fields —
     * clear process_t.querying so a process_destroy_call() can proceed. The
     * kernel-half process_t lives in the heap (PTE_U=0) and faults if
     * dereferenced from U-mode roottask, so the clear is a tiny PRIV kernel
     * extension that resolves the pid to a process_t in kernel VA and writes
     * the field there.
     */
    syscall_priv(__KSYS_PRIV_QUERY_OBJECT_DONE, (void *) (uintptr_t) pid);
}

#include "root-process.h"

/* process_query_info — take a combined snapshot of a process's kernel-half and
 * roottask-half (root_process_t) state.
 *
 * The kernel-half fields come back through the __KSYS_PRIV_PROC_QUERY kernel
 * extension (the kernel heap is PTE_U=0, so U-mode roottask cannot read the
 * process_t directly). The POSIX-policy fields (pgrp, siginfo, session) live in
 * the roottask-side root_process_t and are filled in here from a direct
 * root_proc(pid) array index.
 *
 * Parameters:
 *   pid  : target process pid
 *   info : buffer that receives the combined snapshot
 * Return value: EOK on success, errno on failure.
 */
int process_query_info(pid_t pid, root_proc_info_t *info)
{
    struct kcall_args_proc_query data;
    int r;

    data.pid = pid;
    data.info = info;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_PROC_QUERY, &data);
    r = data.ret;
    if (r == EOK) {
        /* pgrp / siginfo / session live in root_process_t now (POSIX policy
         * isn't kernel business — see kernel/include/objects.h). Fill them in
         * here so the caller still gets a single combined snapshot;
         * root_proc(pid)->X is a direct array index in roottask BSS, no kernel
         * extension needed.
         */
        root_process_t *tmp = root_proc(pid);
        info->pgrp = tmp->pgrp;
        if (tmp->siginfo.si_signo != 0)
            info->siginfo = tmp->siginfo;
        info->session = tmp->session;
        if (tmp->session) {
            info->sess_leader = tmp->session->leader;
            info->sess_pgrp = tmp->session->pgrp;
            info->sess_fd = tmp->session->fd;
        } else {
            info->sess_fd = -1;
        }
    }
    return r;
}

/* channel_has_receivers — non-zero iff the channel (pid, chid) has at least one
 * thread blocked in msg_receive. Replaces the U-mode-illegal receive_queue
 * pointer deref roottask used during message_start.
 */
struct kcall_args_chan_has_recv {
    pid_t pid;
    int chid;
    int *out;
    int ret;
};

/* channel_has_receivers — test whether channel (pid, chid) has a thread blocked
 * in msg_receive. Wraps __KSYS_PRIV_CHANNEL_HAS_RECEIVERS. Parameters:
 *   pid  : process owning the channel
 *   chid : channel id
 * Return value: non-zero if a receiver is blocked, 0 otherwise.
 */
int channel_has_receivers(pid_t pid, int chid)
{
    struct kcall_args_chan_has_recv data;
    int has = 0;

    data.pid = pid;
    data.chid = chid;
    data.out = &has;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_CHANNEL_HAS_RECEIVERS, &data);
    return (data.ret == EOK) ? has : 0;
}

/* roottask_mmap_phys_user — bootstrap-only mmap helper. Maps
 * paddr..paddr+size-1 into roottask's user VA at a kernel-picked address (or
 * "vaddr_hint" if non-zero). See the slot comment in
 * internal/include/sys/calls.h for why this exists separately from libc mmap().
 *
 * Returns EOK and writes the chosen vaddr into *vaddr_out.
 */
struct kcall_args_root_mmap_phys_user {
    paddr_t paddr;
    size_t size;
    int prot;
    uintptr_t vaddr;
    int ret;
};

/* roottask_mmap_phys_user — map a physical range into roottask's user VA. Wraps
 * __KSYS_PRIV_MMAP_PHYS_USER; see the struct comment above. Parameters:
 *   paddr      : physical base to map
 *   size       : number of bytes to map
 *   prot       : protection flags for the mapping
 *   vaddr_hint : preferred virtual address, or 0 to let the kernel choose
 *   vaddr_out  : if non-NULL, receives the chosen virtual address
 * Return value: EOK on success, errno on failure.
 */
int roottask_mmap_phys_user(paddr_t paddr,
                            size_t size,
                            int prot,
                            uintptr_t vaddr_hint,
                            uintptr_t *vaddr_out)
{
    struct kcall_args_root_mmap_phys_user data;

    data.paddr = paddr;
    data.size = size;
    data.prot = prot;
    data.vaddr = vaddr_hint;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_MMAP_PHYS_USER, &data);
    if (data.ret == EOK && vaddr_out) {
        *vaddr_out = data.vaddr;
    }
    return data.ret;
}

/* roottask_mmap_anon_user — anonymous sister of roottask_mmap_phys_user. Used
 * to allocate the pool-thread stacks during message_start, before
 * thread_pool_start spawns receivers and libc mmap could deadlock.
 */
struct kcall_args_root_mmap_anon_user {
    size_t size;
    int prot;
    uintptr_t vaddr;
    int ret;
};

/* roottask_mmap_anon_user — map anonymous memory into roottask's user VA. Wraps
 * __KSYS_PRIV_MMAP_ANON_USER; see the struct comment above. Parameters:
 *   size      : number of bytes to map
 *   prot      : protection flags for the mapping
 *   vaddr_out : if non-NULL, receives the chosen virtual address
 * Return value: EOK on success, errno on failure.
 */
int roottask_mmap_anon_user(size_t size, int prot, uintptr_t *vaddr_out)
{
    struct kcall_args_root_mmap_anon_user data;

    data.size = size;
    data.prot = prot;
    data.vaddr = 0;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_MMAP_ANON_USER, &data);
    if (data.ret == EOK && vaddr_out) {
        *vaddr_out = data.vaddr;
    }
    return data.ret;
}

/* kernel_page_zero — zero "len" bytes of kernel-direct-map memory backing the
 * given paddr. See kext_kpage_zero in kernel/ext/as-memcpy.c for the rationale:
 * U-mode roottask cannot write the kernel-direct-map (PTE_U=0), so the actual
 * memset has to happen in S-mode. Used by pq_init_run after pa_alloc.
 */
struct kcall_args_kpage_zero {
    paddr_t paddr;
    size_t len;
};

/* kernel_page_zero — zero kernel-direct-map memory backing a physical range.
 * Wraps __KSYS_PRIV_KPAGE_ZERO; see the struct comment above. Parameters:
 *   paddr : physical base of the region to zero
 *   len   : number of bytes to zero
 * Return value: EOK on success, errno on failure.
 */
int kernel_page_zero(paddr_t paddr, size_t len)
{
    struct kcall_args_kpage_zero data;

    data.paddr = paddr;
    data.len = len;
    return (int) syscall_priv(__KSYS_PRIV_KPAGE_ZERO, &data);
}

/* kernel_icache_sync_all — broadcast fence.i to every hart (slot
 * __KSYS_PRIV_ICACHE_SYNC). Call after writing a program's text into
 * freshly-allocated pages so the hart that later executes it does not fetch a
 * recycled page's stale instructions. Takes no arguments; a dummy data word is
 * passed because the dispatcher always hands the kernel extension a pointer.
 */
int kernel_icache_sync_all(void)
{
    unsigned long dummy = 0;

    return (int) syscall_priv(__KSYS_PRIV_ICACHE_SYNC, &dummy);
}

/* process_set_spawn — publish a child's entry state and apply spawn-flag
 * inheritance, kernel-side.
 *
 * Pre-publishes initial_ip / initial_sp on a child's prp before the loader
 * sends SPAWN_DONE, and applies the SPAWN_EXEC / SPAWN_NOZOMBIE inheritance
 * rules.
 *
 * Returns the (possibly-augmented) caller-side spawn flags via
 * *spawn_flags_inout so the loader can keep its lcp->ctx.flags in sync with the
 * kernel-side bookkeeping.
 *
 * The kernel heap is mapped PTE_U=0, so U-mode roottask cannot touch
 * prp->initial_ip / prp->flags directly — hence this kernel extension.
 */
struct kcall_args_proc_set_spawn {
    pid_t pid;
    uintptr_t initial_ip;
    uintptr_t initial_sp;
    unsigned spawn_flags_in;
    unsigned spawn_flags_out;
};

/* process_set_spawn — publish a child's entry state and apply spawn-flag
 * inheritance kernel-side. Wraps __KSYS_PRIV_PROC_SET_SPAWN; see the struct
 * comment above. Parameters:
 *   pid                : target child process pid
 *   initial_ip         : initial instruction pointer to publish
 *   initial_sp         : initial stack pointer to publish
 *   spawn_flags_inout  : in/out spawn flags, updated with kernel-side bits
 * Return value: EOK on success, errno on failure.
 */
int process_set_spawn(pid_t pid,
                      uintptr_t initial_ip,
                      uintptr_t initial_sp,
                      unsigned *spawn_flags_inout)
{
    struct kcall_args_proc_set_spawn data;
    int ret;

    data.pid = pid;
    data.initial_ip = initial_ip;
    data.initial_sp = initial_sp;
    data.spawn_flags_in = *spawn_flags_inout;
    data.spawn_flags_out = *spawn_flags_inout;
    ret = (int) syscall_priv(__KSYS_PRIV_PROC_SET_SPAWN, &data);
    if (ret == EOK)
        *spawn_flags_inout = data.spawn_flags_out;
    return ret;
}

/* ProcessListChildren — fill out_pids[] with the children of parent_pid, up to
 * max_pids entries.
 *
 * Returns number actually written via *out_count (also passed back via the
 * kcall_args payload, since the syscall ABI returns EOK / ESRCH). Used by
 * proc_wait to enumerate parent->child without dereferencing the
 * kernel-allocated process_t pointers.
 */
struct kcall_args_proc_list_children {
    pid_t parent_pid;
    pid_t *out_pids;
    size_t max_pids;
    size_t *out_count;
};

/* ProcessListChildren — enumerate a parent's children kernel-side. Wraps
 * __KSYS_PRIV_PROC_LIST_CHILDREN; see the struct comment above. Parameters:
 *   parent_pid : parent process pid
 *   out_pids   : buffer that receives the child pids
 *   max_pids   : capacity of out_pids, in entries
 *   out_count  : receives the number of pids actually written
 * Return value: EOK on success, ESRCH if the parent is gone.
 */
int ProcessListChildren(pid_t parent_pid,
                        pid_t *out_pids,
                        size_t max_pids,
                        size_t *out_count)
{
    struct kcall_args_proc_list_children data;

    data.parent_pid = parent_pid;
    data.out_pids = out_pids;
    data.max_pids = max_pids;
    data.out_count = out_count;
    return (int) syscall_priv(__KSYS_PRIV_PROC_LIST_CHILDREN, &data);
}

/* ProcessWaitFinalize — apply proc_wait_check's writes kernel-side. On return
 * *ret_match holds the (possibly downgraded) match value, and
 * *send_term_notification is 1 if the caller should msg_send_notification
 * PROC_CODE_TERM with priority *terming_priority for child_pid.
 */
struct kcall_args_proc_wait_finalize {
    pid_t child_pid;
    pid_t parent_pid;
    int match;
    int ret_match;
    int send_term_notification;
    unsigned terming_priority;
};

/* ProcessWaitFinalize — apply proc_wait_check's writes kernel-side. Wraps
 * __KSYS_PRIV_PROC_WAIT_FINALIZE; see the struct comment above. Parameters:
 *   child_pid        : the child being waited on
 *   parent_pid       : the waiting parent
 *   match            : the proposed match value
 *   ret_match        : receives the (possibly downgraded) match value
 *   send_term_notification  : receives 1 if the caller should send a TERM
 * notification terming_priority : receives the priority to use for the TERM
 * notification Return value: EOK on success, errno on failure.
 */
int ProcessWaitFinalize(pid_t child_pid,
                        pid_t parent_pid,
                        int match,
                        int *ret_match,
                        int *send_term_notification,
                        unsigned *terming_priority)
{
    struct kcall_args_proc_wait_finalize data;
    int r;

    data.child_pid = child_pid;
    data.parent_pid = parent_pid;
    data.match = match;
    data.ret_match = match;
    data.send_term_notification = 0;
    data.terming_priority = 0;
    r = (int) syscall_priv(__KSYS_PRIV_PROC_WAIT_FINALIZE, &data);
    if (ret_match)
        *ret_match = data.ret_match;
    if (send_term_notification)
        *send_term_notification = data.send_term_notification;
    if (terming_priority)
        *terming_priority = data.terming_priority;
    return r;
}

/* ProcessNozombieMark — stamp a child's siginfo + set WAITINFO/NOZOMBIE. (Field
 * names avoid si_*: those are macros from <sys/siginfo.h>.)
 */
struct kcall_args_proc_nozombie_mark {
    pid_t pid;
    int signo;
    int code;
    pid_t si_proc_pid;
    int status;
};

/* ProcessNozombieMark — stamp a child's siginfo and set WAITINFO/NOZOMBIE.
 * Wraps __KSYS_PRIV_PROC_NOZOMBIE_MARK; see the struct comment above.
 * Parameters:
 *   pid           : target child process pid
 *   signo         : signal number to record
 *   code          : signal code to record
 *   pid_for_si    : sending pid recorded in the siginfo
 *   status_for_si : exit status recorded in the siginfo
 * Return value: EOK on success, errno on failure.
 */
int ProcessNozombieMark(pid_t pid,
                        int signo,
                        int code,
                        pid_t pid_for_si,
                        int status_for_si)
{
    struct kcall_args_proc_nozombie_mark data;

    data.pid = pid;
    data.signo = signo;
    data.code = code;
    data.si_proc_pid = pid_for_si;
    data.status = status_for_si;
    return (int) syscall_priv(__KSYS_PRIV_PROC_NOZOMBIE_MARK, &data);
}

/* ProcessExitMark — stamp prp at the start of process exit. Wrapper for
 * __KSYS_PRIV_PROC_EXIT_MARK; see kext_proc_exit_mark.
 */
struct kcall_args_proc_exit_mark {
    pid_t pid;
    int exit_status;
};

/* ProcessExitMark — stamp a process at the start of exit. Wraps
 * __KSYS_PRIV_PROC_EXIT_MARK; see the struct comment above. Parameters:
 *   pid         : exiting process pid
 *   exit_status : exit status to record
 * Return value: EOK on success, errno on failure.
 */
int ProcessExitMark(pid_t pid, int exit_status)
{
    struct kcall_args_proc_exit_mark data;

    data.pid = pid;
    data.exit_status = exit_status;
    return (int) syscall_priv(__KSYS_PRIV_PROC_EXIT_MARK, &data);
}

/* ProcessDestroyChancons — bulk teardown of all chancons entries on a process
 * matching the given type (TYPE_CHANNEL or TYPE_CONNECTION). See
 * kernel/ext/exit.c:kext_proc_destroy_chancons.
 */
struct kcall_args_proc_destroy_chancons {
    pid_t pid;
    int type_filter;
};

/* ProcessDestroyChancons — tear down all of a process's chancons entries
 * matching a type filter. Wraps __KSYS_PRIV_PROC_DESTROY_CHANCONS; see the
 * struct comment above. Parameters:
 *   pid         : owning process pid
 *   type_filter : TYPE_CHANNEL or TYPE_CONNECTION to tear down
 * Return value: EOK on success, errno on failure.
 */
int ProcessDestroyChancons(pid_t pid, int type_filter)
{
    struct kcall_args_proc_destroy_chancons data;

    data.pid = pid;
    data.type_filter = type_filter;
    return (int) syscall_priv(__KSYS_PRIV_PROC_DESTROY_CHANCONS, &data);
}

/* ProcessDestroyFdcons — bulk teardown of all fdcons entries on a process. See
 * kernel/ext/exit.c:kext_proc_destroy_fdcons.
 */
struct kcall_args_proc_destroy_fdcons {
    pid_t pid;
};

int ProcessDestroyFdcons(pid_t pid)
{
    struct kcall_args_proc_destroy_fdcons data;

    data.pid = pid;
    return (int) syscall_priv(__KSYS_PRIV_PROC_DESTROY_FDCONS, &data);
}

/* ProcessClearPls — clear prp->pls = NULL. Used by proc_exit's memory-teardown
 * step.
 */
int ProcessClearPls(pid_t pid)
{
    return (int) syscall_priv(__KSYS_PRIV_PROC_CLEAR_PLS,
                              (void *) (uintptr_t) pid);
}

/* process_clear_lcp — clear prp->lcp = NULL. Used by loader_exit. */
int process_clear_lcp(pid_t pid)
{
    return (int) syscall_priv(__KSYS_PRIV_PROC_CLEAR_LCP,
                              (void *) (uintptr_t) pid);
}

/* ProcessIndexToPid — process_vector[index] -> prp->pid (full pid).
 * Returns 0 if the slot is empty. Used by /proc directory readers iterating via
 * query_object which yields kernel-VA prp pointers.
 */
struct kcall_args_proc_index_to_pid {
    unsigned index;
    pid_t pid_out;
};

/* ProcessIndexToPid — translate a process_vector slot index to a full pid.
 * Wraps __KSYS_PRIV_PROC_INDEX_TO_PID; see the struct comment above.
 * Parameters:
 *   index : process_vector slot index
 * Return value: the full pid, or 0 if the slot is empty.
 */
pid_t ProcessIndexToPid(unsigned index)
{
    struct kcall_args_proc_index_to_pid data;

    data.index = index;
    data.pid_out = 0;
    (void) syscall_priv(__KSYS_PRIV_PROC_INDEX_TO_PID, &data);
    return data.pid_out;
}

/* process_set_ocb_list — set prp->ocb_list = head. Reads come via
 * process_query_info's "ocb_list" field; writes through this kernel extension.
 */
struct kcall_args_proc_set_ocb_list {
    pid_t pid;
    void *head;
};

/* process_set_ocb_list — set a process's ocb_list head kernel-side. Wraps
 * __KSYS_PRIV_PROC_SET_OCB_LIST; see the struct comment above. Parameters:
 *   pid  : owning process pid
 *   head : new ocb_list head pointer
 * Return value: EOK on success, errno on failure.
 */
int process_set_ocb_list(pid_t pid, void *head)
{
    struct kcall_args_proc_set_ocb_list data;

    data.pid = pid;
    data.head = head;
    return (int) syscall_priv(__KSYS_PRIV_PROC_SET_OCB_LIST, &data);
}

/* ProcessExitFinalize — final exit fixups (clear TERMING | PRIV). See
 * kext_proc_exit_finalize.
 */
struct kcall_args_proc_exit_finalize {
    pid_t pid;
};

/* ProcessExitFinalize — apply the final exit fixups for a process. Wraps
 * __KSYS_PRIV_PROC_EXIT_FINALIZE; see the struct comment above. Parameters:
 *   pid : exiting process pid
 * Return value: EOK on success, errno on failure.
 */
int ProcessExitFinalize(pid_t pid)
{
    struct kcall_args_proc_exit_finalize data;

    data.pid = pid;
    return (int) syscall_priv(__KSYS_PRIV_PROC_EXIT_FINALIZE, &data);
}

/* ProcessFlagsOp — generic prp->flags |= set, prp->flags &= ~clear. Heap is
 * PTE_U=0; this kernel extension keeps the read-modify-write inside the kernel
 * where prp is dereferenceable.
 */
struct kcall_args_proc_flags_op {
    pid_t pid;
    uint32_t flags_set;
    uint32_t flags_clear;
    uint32_t flags_after;
};

/* ProcessFlagsOp — set and clear bits in a process's flags kernel-side. Wraps
 * __KSYS_PRIV_PROC_FLAGS_OP; see the struct comment above. Parameters:
 *   pid   : owning process pid
 *   set   : flag bits to OR in
 *   clear : flag bits to clear
 * Return value: EOK on success, errno on failure.
 */
int ProcessFlagsOp(pid_t pid, uint32_t set, uint32_t clear)
{
    struct kcall_args_proc_flags_op data;

    data.pid = pid;
    data.flags_set = set;
    data.flags_clear = clear;
    data.flags_after = 0;
    return (int) syscall_priv(__KSYS_PRIV_PROC_FLAGS_OP, &data);
}

/* ProcessSetRlimitCpu — install RLIMIT_CPU (soft/hard, seconds) for a process
 * kernel-side; the kernel derives the nanosecond max_cpu_time the tick
 * enforces. Wraps __KSYS_PRIV_PROC_SET_RLIMIT.
 *
 * Return value: EOK, or ESRCH if pid is gone.
 */
struct kcall_args_proc_set_rlimit {
    pid_t pid;
    uint64_t cur;
    uint64_t max;
};

int ProcessSetRlimitCpu(pid_t pid, uint64_t cur, uint64_t max)
{
    struct kcall_args_proc_set_rlimit data;

    data.pid = pid;
    data.cur = cur;
    data.max = max;
    return (int) syscall_priv(__KSYS_PRIV_PROC_SET_RLIMIT, &data);
}

/* soul_stats — fetch one record per kernel object pool (see kstats.h), plus the
 * per-hart STIMER interrupt counts into timer_intr (clipped to timer_n entries;
 * *timer_count is the number of online harts).
 * Returns EOK, ERANGE if n is too small (*count_out is still set), or EFAULT.
 */
int soul_stats(struct kstat_soul_rec *out,
               unsigned n,
               unsigned *count_out,
               unsigned *live_threads,
               unsigned *live_timeouts,
               uint64_t *timer_intr,
               unsigned timer_n,
               unsigned *timer_count)
{
    struct kcall_args_soul_stats data;

    data.out = out;
    data.n = n;
    data.count_out = count_out;
    data.live_threads = 0;
    data.live_timeouts = 0;
    data.timer_intr = timer_intr;
    data.timer_n = timer_n;
    data.timer_count = 0;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_SOUL_STATS, &data);
    if (live_threads)
        *live_threads = data.live_threads;
    if (live_timeouts)
        *live_timeouts = data.live_timeouts;
    if (timer_count)
        *timer_count = data.timer_count;
    return data.ret;
}

int ProcessCapStats(pid_t pid, rtmux_cap_stats_t *stats)
{
    struct kcall_args_proc_cap_stats data;

    data.pid = pid;
    data.stats = stats;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_PROC_CAP_STATS, &data);
    return data.ret;
}

int ProcessEndpointCap(pid_t pid,
                       uint32_t coid,
                       rtmux_cap_t *cap,
                       uint32_t *rights)
{
    struct kcall_args_proc_endpoint_cap data;

    data.pid = pid;
    data.coid = coid;
    data.cap = 0;
    data.rights = 0;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_PROC_ENDPOINT_CAP, &data);
    if (data.ret == EOK) {
        if (cap)
            *cap = data.cap;
        if (rights)
            *rights = data.rights;
    }
    return data.ret;
}

/* Mint a process capability over pid into this process's endpoint_caps. Gated
 * on RTMUX_PRIV_CAP_PROCESS, which roottask holds. rights is a subset of
 * RTMUX_CAP_PROCESS_ALL. On success the handle is returned via cap.
 */
int ProcessCapMint(pid_t pid, uint32_t rights, rtmux_cap_t *cap)
{
    struct kcall_args_proc_cap_mint data;

    data.pid = pid;
    data.rights = rights;
    data.cap = 0;
    data.ret = -1;
    (void) syscall_priv(__KSYS_PRIV_PROC_CAP_MINT, &data);
    if (data.ret == EOK && cap)
        *cap = data.cap;
    return data.ret;
}

/* Reboot / Shutdown (reboot.c) */
/* system_reboot — reboot the machine. Does not return. Parameters:
 *   abnormal : non-zero for an abnormal (fault-induced) reboot
 */
void system_reboot(int abnormal)
{
    syscall_priv(__KSYS_PRIV_REBOOT, (void *) (uintptr_t) abnormal);
    /* unreachable */
}

/* system_shutdown — halt the machine. Does not return. */
void system_shutdown(void)
{
    syscall_priv(__KSYS_PRIV_SHUTDOWN, NULL);
    /* unreachable */
}

/* signal_kill (signal.c) */
struct kext_signal_args {
    channel_t *chp;
    int signo;
    pid_t sender_pid;
};

/* signal_kill — deliver a signal to a process by posting a notification to its
 * system channel. Parameters:
 *   chp        : opaque kernel-VA pointer to the target's system channel
 *   signo      : signal number to deliver
 *   sender_pid : pid of the sending process
 */
void signal_kill(channel_t *chp, int signo, pid_t sender_pid)
{
    struct kext_signal_args args = {
        .chp = chp,
        .signo = signo,
        .sender_pid = sender_pid,
    };
    syscall_priv(__KSYS_PRIV_SIGNAL_DELIVER, &args);
}

/* Stack (stack.c) */
struct kcall_args_stack_cont {
    pid_t pid;
    int tid;
    int err;
    struct _thread_attr *attr;
};

struct kcall_args_stack_wait_info {
    union sigval value;
    pid_t *ppid;
    int *ptid;
    struct _thread_attr *attr;
};

/* stack_cont — resume a thread parked awaiting thread-stack allocation,
 * reporting the allocation result. Parameters:
 *   pid  : target process pid
 *   tid  : target thread id
 *   err  : EOK if the stack was allocated, else the failure errno
 *   attr : thread attributes carrying the allocated stack
 * Return value: EOK on success, errno on failure.
 */
int stack_cont(pid_t pid, int tid, int err, struct _thread_attr *attr)
{
    struct kcall_args_stack_cont data;
    data.pid = pid;
    data.tid = tid;
    data.err = err;
    data.attr = attr;
    return (int) syscall_priv(__KSYS_PRIV_STACK_CONT, &data);
}

/* stack_wait_info — fetch the details of a pending thread-stack request.
 * Parameters:
 *   value : notification value identifying the request
 *   ppid  : receives the requesting process pid
 *   ptid  : receives the requesting thread id
 *   attr  : receives the requested thread attributes
 * Return value: EOK on success, -1 on failure.
 */
int stack_wait_info(union sigval value,
                    pid_t *ppid,
                    int *ptid,
                    struct _thread_attr *attr)
{
    struct kcall_args_stack_wait_info data;
    data.value = value;
    data.ppid = ppid;
    data.ptid = ptid;
    data.attr = attr;
    return (int) syscall_priv(__KSYS_PRIV_STACK_WAIT_INFO, &data);
}

/* thread_tls (process.c) */
struct kcall_args_thread_tls {
    struct _thread_local_storage *tls;
    uintptr_t stackaddr;
    size_t stacksize;
    size_t guardsize;
    uintptr_t esp;
};

/* thread_tls — initialize a thread's thread-local-storage block from its stack
 * geometry. Parameters:
 *   tls       : TLS block to initialize
 *   stackaddr : base address of the thread stack
 *   stacksize : usable stack size, in bytes
 *   guardsize : guard-region size, in bytes
 *   esp       : initial stack pointer
 * Return value: the (possibly relocated) TLS block pointer.
 */
struct _thread_local_storage *thread_tls(struct _thread_local_storage *tls,
                                         uintptr_t stackaddr,
                                         size_t stacksize,
                                         size_t guardsize,
                                         uintptr_t esp)
{
    struct kcall_args_thread_tls data;
    data.tls = tls;
    data.stackaddr = stackaddr;
    data.stacksize = stacksize;
    data.guardsize = guardsize;
    data.esp = esp;
    return (struct _thread_local_storage *) syscall_priv(__KSYS_PRIV_THREAD_TLS,
                                                         &data);
}

/* as_memcpy — cross-as memcpy between roottask and a target process's user VA.
 *
 * kernel_exit forces roottask's own satp on every U-mode return, so roottask
 * cannot read/write a foreign user VA directly; this kernel extension does the
 * satp swap plus xfer_memcpy in S-mode instead.
 *
 * "roottask_buf" MUST be a kernel-direct-map VA (>= KERNEL_VIRT_BASE); a U-mode
 * roottask buffer would not survive the satp swap. Most callers obtain such a
 * VA via pte_temp_map() which returns paddr + CPU_1TO1_VADDR_BIAS.
 *
 * Returns EOK / EFAULT / ESRCH / EINVAL.
 */
struct kcall_args_as_memcpy {
    pid_t target_pid;
    void *target_va;
    void *roottask_buf;
    size_t len;
    int direction; /* 0 = read from target, 1 = write to target */
};

/* as_memcpy — copy between roottask and a target process's user VA. Wraps
 * __KSYS_PRIV_AS_MEMCPY; see the block comment above. Parameters:
 *   target_pid  : process owning target_va
 *   target_va   : user virtual address in the target process
 *   roottask_buf : kernel-direct-map buffer (>= KERNEL_VIRT_BASE)
 *   len         : number of bytes to copy
 *   direction   : 0 = read from target, 1 = write to target
 * Return value: EOK / EFAULT / ESRCH / EINVAL.
 */
int as_memcpy(pid_t target_pid,
              void *target_va,
              void *roottask_buf,
              size_t len,
              int direction)
{
    struct kcall_args_as_memcpy data;
    data.target_pid = target_pid;
    data.target_va = target_va;
    data.roottask_buf = roottask_buf;
    data.len = len;
    data.direction = direction;
    return (int) syscall_priv(__KSYS_PRIV_AS_MEMCPY, &data);
}

/* RegisterPoolStack — push a user-VA stack page (from as_mmap) onto the
 * kernel-resident roottask_stack_cache free list. Called from message_start() N
 * times to pre-populate the pool cache before thread_pool_start fires off the
 * first batch of pool threads.
 */
struct kcall_args_register_pool_stack {
    void *vaddr;
    size_t size;
};

/* RegisterPoolStack — push a user-VA stack page onto the kernel-resident
 * roottask_stack_cache free list. Wraps __KSYS_PRIV_REGISTER_POOL_STACK; see
 * the block comment above. Parameters:
 *   vaddr : base of the stack page to register
 *   size  : size of the stack region, in bytes
 * Return value: EOK on success, errno on failure.
 */
int RegisterPoolStack(void *vaddr, size_t size)
{
    struct kcall_args_register_pool_stack data;
    data.vaddr = vaddr;
    data.size = size;
    return (int) syscall_priv(__KSYS_PRIV_REGISTER_POOL_STACK, &data);
}

/* kext_add_trace_event_iov — roottask-side wrapper for the trace-event
 * submission path. The kernel provides an identical function in trace.c (also
 * gated on CONFIG_INSTRUMENTATION). U-mode roottask is a separate ELF and must
 * do the syscall_priv(__KSYS_PRIV_TRACE, ...) hop itself, just like the other
 * Group-B wrappers in this file.
 *
 * The kcall_args_trace_args layout MUST match kernel/ext/trace.c (see
 * file-header comment above re lockstep struct duplication).
 */
struct kcall_args_trace_args {
    int ev_class;
    int ev_code;
    int iov_cnt;
    iov_t *iovs;
};

/* kext_add_trace_event_iov — submit a trace event from an IOV vector. Wraps
 * __KSYS_PRIV_TRACE; compiles to a no-op when CONFIG_INSTRUMENTATION is not
 * defined. See the block comment above. Parameters:
 *   ev_class : trace event class
 *   ev_code  : trace event code
 *   iovs     : IOV array describing the event payload
 *   iovcnt   : number of entries in iovs
 * Return value: kernel trace result, or 0 when instrumentation is off.
 */
int kext_add_trace_event_iov(int ev_class, int ev_code, iov_t *iovs, int iovcnt)
{
#if defined(CONFIG_INSTRUMENTATION)
    struct kcall_args_trace_args args = {
        .ev_class = ev_class,
        .ev_code = ev_code,
        .iov_cnt = iovcnt,
        .iovs = iovs,
    };
    return (int) syscall_priv(__KSYS_PRIV_TRACE, &args);
#else
    (void) ev_class;
    (void) ev_code;
    (void) iovs;
    (void) iovcnt;
    return 0;
#endif
}
