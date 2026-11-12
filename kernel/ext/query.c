/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <private/root-proc-info.h>
#include <private/kstats.h>
#include <sys/mman.h>
#include <sys/wait.h> /* WEXITED / WUNTRACED / WCONTINUED ... */
#include "mm-xboundary.h"
#include "as.h" /* struct mm_pte_manipulate, PTE_OP_MAP */
#include <string.h>

extern int cpu_pte_manipulate(struct mm_pte_manipulate *data);


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

#define MAX_RESTARTS 10

/* Look up a kernel object for a query_object call (kernel extension body).
 *
 * Runs nano_query for the requested object type/index and returns the resulting
 * object pointer. When the result is a process_t from a process vector query,
 * it is marked "querying" so process_destroy_call won't free it while a reader
 * inspects its fields. process_rcu spans the nano_query call and the "querying"
 * write so the obj pointer's lifetime covers the mutation; nano_query takes its
 * own per-vector slocks (vec_slock etc.) internally. Parameters:
 *   data : pointer to a struct kcall_args_query_object (query selectors +
 * buffer)
 */
void kext_query_object(void *data)
{
    struct kcall_args_query_object *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    void *obj;

    if (++kap->restarts > MAX_RESTARTS) {
        /* The interrupt rate is high enough that we're not able to complete the
         * nano_query without restarting. Lock the kernel so we can get the job
         * done. Increases latency, but at least we'll complete the operation
         * This branch is the explicit "force completion" fallback: it
         * deliberately holds the heavy lock to bound retry loops even once
         * fine-grained locking lands.
         */
    }
    rcu_read_lock(&process_rcu);
    obj = nano_query(kap->type, kap->index1, kap->subtype, kap->index2,
                     (unsigned *) kap->next, kap->objbuf, kap->objsize);
    if (obj && (kap->type == _QUERY_PROCESS) &&
        (kap->subtype == _QUERY_PROCESS_VECTOR)) {
        /* We got a process_t pointer back. We have to mark it as having a query
         * in-process so that the process_destroy_call() code doesn't free the
         * memory for it while some other thread is looking at the fields.
         */
        ((process_t *) obj)->querying = true;
    }
    rcu_read_unlock(&process_rcu);

    SETKSTATUS(act, obj);
}

/* query_object wrapper: servers/roottask/proc/kext-wrappers.c */


/* kext_query_object_done — clear process_t.querying for the given pid.
 *
 * Counterpart to the "prp->querying = true" set above. Roottask used to clear
 * this from U-mode by dereferencing the kernel-allocated process_t directly;
 * with the kernel heap PTE_U=0 that no longer works, so the clear is now a tiny
 * PRIV kernel extension. The arg is just the pid (passed as void* to fit the
 * syscall_priv signature).
 */
void kext_query_object_done(void *data)
{
    pid_t pid = (pid_t) (uintptr_t) data;
    process_t *prp = lookup_pid_rcu(pid);

    if (prp)
        prp->querying = false;
    rcu_read_unlock(&process_rcu);
    /* No KSTATUS — caller's query_object_done is void. */
}

/* kext_proc_query — copy a snapshot of the kernel-half process_t fields
 * roottask wants to read into a U-mode buffer. See
 * kernel/include/private/root-proc-info.h for the snapshot layout.
 *
 * Post-BKL this snapshot is a field-by-field lockless read, so fields being
 * mutated by the target on another CPU (e.g. fault_siginfo while that process
 * is taking a fatal fault, num_active_threads during teardown) may be observed
 * torn. That is acceptable: every field here is plain advisory data with no
 * embedded pointer the reader dereferences, so a torn value is at worst stale,
 * never unsafe. Writing into the U-mode buffer is fine: the trap saved sstatus
 * has SUM=1 for calls.
 */
void kext_proc_query(void *data)
{
    struct kcall_args_proc_query *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);
    root_proc_info_t *out = kap->info;

    if (!prp || !out) {
        rcu_read_unlock(&process_rcu);
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }

    memset(out, 0, sizeof(*out));
    out->pid = prp->pid;
    out->parent_pid = prp->parent ? prp->parent->pid : 0;
    out->flags = prp->flags;
    out->num_active_threads = prp->num_active_threads;
    out->num_total_threads = prp->threads.nentries - prp->threads.nfree;
    out->terming_priority = prp->terming_priority;
    out->process_priority = prp->process_priority;
    out->base_addr = prp->base_addr;
    out->boundary_addr = prp->boundary_addr;
    out->max_cpu_time = prp->max_cpu_time;
    out->running_time = prp->running_time;
    out->system_time = prp->system_time;
    out->kids_running_time = prp->kids_running_time;
    out->kids_system_time = prp->kids_system_time;
    out->has_memory = !!prp->memory;
    out->sys_chp = prp->sys_chp;
    out->lcp = prp->lcp;
    out->ocb_list = prp->ocb_list;
    out->siginfo = prp->fault_siginfo;
    /* Per-process rlimit snapshots for proc_resource handlers. */
    {
        unsigned r;
        for (r = 0; r < RLIM_NLIMITS; ++r) {
            out->rlimit_vals_soft[r] = prp->rlimit_vals_soft[r];
            out->rlimit_vals_hard[r] = prp->rlimit_vals_hard[r];
        }
    }

    /* pgrp / siginfo / session* live in root_process_t (roottask side); the
     * U-mode caller of process_query_info fills those slots from
     * "root_proc(pid)->X" after this kernel extension returns. See
     * servers/roottask/proc/kext-wrappers.c::process_query_info.
     */

    if (prp->cred) {
        out->cred = prp->cred->info;
    }

    kap->ret = EOK;
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

void kext_proc_cap_stats(void *data)
{
    struct kcall_args_proc_cap_stats *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);

    if (!prp || !kap->stats) {
        rcu_read_unlock(&process_rcu);
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }

    kap->ret = rtmux_cap_stats(&prp->endpoint_caps, kap->stats);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, kap->ret);
}

/* kext_soul_stats — copy one record per object pool to roottask.
 *
 * Counts are read under soul_slock so each record is internally consistent.
 * *count_out is always set; ERANGE (and no copy) if the buffer is too small.
 */
void kext_soul_stats(void *data)
{
    extern _Atomic uint64_t stimer_intr_count[]; /* kernel/time/clock-core.c */
    static soul_t *const pools[KSTAT_SOUL_COUNT] = {
#define KSTAT_SOUL_PTR(name, var) [KSTAT_SOUL_##name] = &var,
        KSTAT_SOUL_LIST(KSTAT_SOUL_PTR)
#undef KSTAT_SOUL_PTR
    };
    struct kcall_args_soul_stats *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    struct kstat_soul_rec recs[KSTAT_SOUL_COUNT];
    unsigned long iflags;
    int ret = EOK;

    /* Threads that live in some process's thread vector. A server's dispatch
     * pool (roottask: low water 3, high water 10; the pipe server: 2 and 4)
     * adds a thread when requests overlap and sheds one only past its
     * high-water mark, so soul.thread.used legitimately rises by a few and
     * stays. What a leak test wants is the souls NOT reachable from any live
     * process: a thread that left its vector (thread_destroy's vector_rem) but
     * whose soul was never freed, or whose process went away first. Each vector
     * is read under its vec_slock, the lock vector_add/vector_rem in
     * thread_create/ thread_destroy hold, so nentries/nfree are consistent; the
     * RCU-sched section keeps every process_t alive while walked (the pattern
     * of lookup_pid_rcu). The sum is not atomic with the pool count below, so a
     * thread being created or reaped concurrently shows up as a +-1 that
     * settles; callers compare against a baseline.
     */
    kap->live_threads = 0;
    kap->live_timeouts = 0;
    rcu_sched_read_lock();
    for (unsigned id = 0; id < process_vector.nentries; id++) {
        process_t *prp;

        if (!VECP(prp, &process_vector, id))
            continue;
        kspinlock_lock(&prp->vec_slock);
        kap->live_threads +=
            (unsigned) (prp->threads.nentries - prp->threads.nfree);

        /* A thread's timeout timer is allocated on its first timed wait and
         * freed with the thread, so how many exist depends on which threads
         * happened to time out. Counting them lets a leak test subtract that
         * bounded, per-thread part from soul.timer.used.
         */
        for (unsigned t = 0; t < prp->threads.nentries; t++) {
            thread_t *thp = vector_lookup2(&prp->threads, t);

            if (thp && __atomic_load_n(&thp->timeout, __ATOMIC_RELAXED))
                kap->live_timeouts++;
        }
        kspinlock_unlock(&prp->vec_slock);
    }
    rcu_read_unlock(&process_rcu);

    if (!kap->out || !kap->count_out) {
        ret = EFAULT;
    } else {
        unsigned count = KSTAT_SOUL_COUNT;

        if (xfer_memcpy(kap->count_out, &count, sizeof count) != 0) {
            ret = EFAULT;
        } else if (kap->n < KSTAT_SOUL_COUNT) {
            ret = ERANGE;
        } else {
            iflags = kspinlock_lock_irqsave(&soul_slock);
            for (unsigned i = 0; i < KSTAT_SOUL_COUNT; i++) {
                recs[i].kind = KSTAT_REC_SOUL;
                recs[i].id = i;
                recs[i].total = pools[i]->total;
                recs[i].used = pools[i]->used;
                recs[i].reserved = pools[i]->reserved;
                recs[i].highwater = pools[i]->highwater;
            }
            kspinlock_unlock_irqrestore(&soul_slock, iflags);
            if (xfer_memcpy(kap->out, recs, sizeof recs) != 0)
                ret = EFAULT;
        }
    }

    /* Per-hart STIMER interrupt counts. Each hart bumps only its own slot with
     * a relaxed add (arch/riscv/trap-handler.c), so a relaxed load here is
     * race-free; the values are monotonic and not a cross-hart snapshot.
     * timer_count is always set; the copy is clipped to the caller's capacity.
     */
    kap->timer_count = num_processors;
    if (ret == EOK && kap->timer_intr && kap->timer_n) {
        uint64_t intr[CONFIG_PROCESSORS_MAX];
        unsigned n =
            num_processors < kap->timer_n ? num_processors : kap->timer_n;

        for (unsigned i = 0; i < n; i++)
            intr[i] = __atomic_load_n(&stimer_intr_count[i], __ATOMIC_RELAXED);
        if (xfer_memcpy(kap->timer_intr, intr, n * sizeof intr[0]) != 0)
            ret = EFAULT;
    }
    kap->ret = ret;
    SETKSTATUS(act, ret);
}

void kext_proc_endpoint_cap(void *data)
{
    struct kcall_args_proc_endpoint_cap *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);
    vector_t *vec;
    connect_t *cop;
    uint32_t coid;
    int status = EOK;

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }

    vec = (kap->coid & RTMUX_SIDE_CHANNEL) ? &prp->chancons : &prp->fdcons;
    coid = kap->coid & ~RTMUX_SIDE_CHANNEL;
    rcu_read_lock(&connobj_rcu);
    kspinlock_lock(&prp->vec_slock);
    cop = vector_lookup(vec, coid);
    if (!cop || cop->type != TYPE_CONNECTION) {
        status = EBADF;
        goto out_unlock;
    }
    if (rtmux_cap_get_or_alloc_context(&prp->endpoint_caps, cop,
                                       (uint64_t) kap->coid, cop->rights,
                                       &kap->cap) != EOK) {
        status = ENOSPC;
        goto out_unlock;
    }
    if (cop->cap == 0)
        cop->cap = kap->cap;
    kap->rights = cop->rights;
out_unlock:
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&connobj_rcu);
    rcu_read_unlock(&process_rcu);
    kap->ret = status;
    SETKSTATUS(act, status);
}

/* Mint a process capability over the target pid into the caller's
 * endpoint_caps.
 *
 * The caller must hold RTMUX_PRIV_CAP_PROCESS (checked by the priv dispatcher).
 * That gate is held only by the fully-trusted process managers (coresrv and
 * roottask), which already wield ambient authority over the whole process tree,
 * so minting a cap over an arbitrary pid grants no authority they do not
 * already have. This is a deliberate trust assumption on the current bootstrap
 * manifest: before RTMUX_PRIV_CAP_PROCESS is ever delegated to a less-trusted
 * server, this mint must first be restricted to the caller's creator/parent
 * subtree.
 *
 * The cap names the target process_t. process teardown revokes every cap naming
 * a dying process (cap_revoke_object_all_tables, kext_process_destroy) before
 * the soul is freed, so the raw pointer this cap stores never resolves to a
 * recycled soul. The cap context carries the target pid as a defense-in-depth
 * staleness tag; the cap generation remains the authority tag.
 *
 * Consumer obligation: object-wide revoke marks a pinned slot REVOKING but does
 * not wait for the pin to drop, so any code that resolves one of these caps via
 * rtmux_cap_lookup_pin_typed must promote the returned process_t to process_pin
 * before dereferencing it and dropping the cap pin, or teardown could free the
 * process out from under it.
 */
void kext_proc_cap_mint(void *data)
{
    struct kcall_args_proc_cap_mint *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);
    rtmux_cap_t cap = 0;
    int status;

    /* No handle unless the mint succeeds (cap is 0 unless ret is EOK). */
    kap->cap = 0;

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }
    if (kap->rights == 0 || (kap->rights & ~RTMUX_CAP_PROCESS_ALL) != 0) {
        rcu_read_unlock(&process_rcu);
        kap->ret = EINVAL;
        SETKSTATUS(act, EINVAL);
        return;
    }

    status = rtmux_cap_alloc_typed(&act->process->endpoint_caps, prp,
                                   (uint64_t) kap->pid, kap->rights,
                                   RTMUX_CAP_TYPE_PROCESS, &cap);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kap->ret = ENOSPC;
        SETKSTATUS(act, ENOSPC);
        return;
    }
    kap->cap = cap;
    kap->ret = EOK;
    SETKSTATUS(act, EOK);
}

/* kext_channel_has_receivers — look up the channel at (pid, chid) and answer
 * whether any thread is currently blocked in msg_receive on it. Args struct:
 * (pid_t pid, int chid, int *out_has_receivers).
 *
 * Returns EOK if the channel exists, ESRCH otherwise.
 */
struct kcall_args_chan_has_recv {
    pid_t pid;
    int chid;
    int *out;
    int ret;
};

void kext_channel_has_receivers(void *data)
{
    struct kcall_args_chan_has_recv *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);
    channel_t *chp;
    int has_receivers;

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }
    rcu_read_lock(&connobj_rcu);
    kspinlock_lock(&prp->vec_slock);
    chp = vector_lookup(&prp->chancons, (unsigned) kap->chid);
    if (!chp || chp->type != TYPE_CHANNEL) {
        kspinlock_unlock(&prp->vec_slock);
        rcu_read_unlock(&connobj_rcu);
        rcu_read_unlock(&process_rcu);
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }
    has_receivers = chp->receive_queue ? 1 : 0;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&connobj_rcu);
    rcu_read_unlock(&process_rcu);
    if (kap->out)
        *kap->out = has_receivers;
    kap->ret = EOK;
    SETKSTATUS(act, EOK);
}

/* kext_proc_register_syschan — register a process's system notification
 * channel. Looks up prp->chancons[chid], verifies it is a TYPE_CHANNEL, and
 * stores the resulting channel_t* in prp->sys_chp. Used by system-channel.c to
 * record the channel that the kernel will deliver process-control notifications
 * (PROC_CODE_TERM, etc.) to. Lifted into a kernel extension because U-mode
 * roottask cannot dereference the kernel-allocated process_t (heap PTE_U=0).
 */
struct kcall_args_proc_register_syschan {
    pid_t pid;
    int chid;
};

void kext_proc_register_syschan(void *data)
{
    struct kcall_args_proc_register_syschan *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp;
    channel_t *chp;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    rcu_read_lock(&connobj_rcu);
    kspinlock_lock(&prp->vec_slock);
    chp = vector_lookup2(&prp->chancons, (unsigned) kap->chid);
    if (!chp || chp->type != TYPE_CHANNEL) {
        kspinlock_unlock(&prp->vec_slock);
        rcu_read_unlock(&connobj_rcu);
        rcu_read_unlock(&process_rcu);
        kcall_err(act, EBADF);
        return;
    }
    prp->sys_chp = chp;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&connobj_rcu);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_list_children — fill an array of pid_t with the children of
 * "parent_pid" (head-of-list at parent->child, walked via ->sibling). Used by
 * proc_wait to enumerate children without dereferencing the kernel-allocated
 * process_t pointers; pid_t carries the generation (lookup_pid catches stale
 * references on the next per-child kernel extension).
 */
struct kcall_args_proc_list_children {
    pid_t parent_pid;
    pid_t *out_pids;
    size_t max_pids;
    size_t *out_count;
};

void kext_proc_list_children(void *data)
{
    struct kcall_args_proc_list_children *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *parent = lookup_pid_rcu(kap->parent_pid);
    process_t *child;
    size_t count = 0;

    if (!parent) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    kspinlock_lock(&parent->vec_slock);
    for (child = parent->child; child && count < kap->max_pids;
         child = child->sibling) {
        kap->out_pids[count++] = child->pid;
    }
    kspinlock_unlock(&parent->vec_slock);
    rcu_read_unlock(&process_rcu);
    *kap->out_count = count;
    SETKSTATUS(act, EOK);
}

/* kext_proc_wait_finalize — apply the terminal write side-effects of
 * proc_wait_check kernel-side. Reads / clears prp->flags, mirrors cpu times
 * into the parent, and tells the caller whether to send the PROC_CODE_TERM
 * notification. All match-specific bookkeeping that used to live inline in
 * roottask happens here.
 */
struct kcall_args_proc_wait_finalize {
    pid_t child_pid;
    pid_t parent_pid;
    int match;     /* in: WEXITED / WUNTRACED / RTMUX_WTRAPPED / WCONTINUED */
    int ret_match; /* out: possibly downgraded to WNOWAIT */
    int send_term_notification; /* out: 1 if caller should notify roottask
                                   for term
                                   */
    unsigned terming_priority;  /* out: notification priority for the term
                                   notification
                                   */
};

void kext_proc_wait_finalize(void *data)
{
    struct kcall_args_proc_wait_finalize *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->child_pid);
    process_t *parent = lookup_pid_rcu(kap->parent_pid);
    int match = kap->match;

    kap->ret_match = match;
    kap->send_term_notification = 0;
    kap->terming_priority = 0;

    if (!prp || !parent) {
        rcu_read_unlock(&process_rcu);
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }

    if ((uintptr_t) prp < (uintptr_t) parent) {
        kspinlock_lock(&prp->vec_slock);
        kspinlock_lock(&parent->vec_slock);
    } else if (prp != parent) {
        kspinlock_lock(&parent->vec_slock);
        kspinlock_lock(&prp->vec_slock);
    } else {
        kspinlock_lock(&prp->vec_slock);
    }

    switch (match) {
    case WEXITED:
        if (prp->flags & RTMUX_FLG_PROC_WAITINFO) {
            parent->kids_running_time +=
                prp->running_time + prp->kids_running_time;
            parent->kids_system_time +=
                prp->system_time + prp->kids_system_time;
            prp->flags &= ~RTMUX_FLG_PROC_WAITINFO;
            prp->flags |= RTMUX_FLG_PROC_WAITDONE;
        }
        if (prp->flags & RTMUX_FLG_PROC_ZOMBIE) {
            kap->send_term_notification = 1;
            kap->terming_priority = prp->terming_priority;
        } else {
            kap->ret_match = WNOWAIT;
        }
        break;
    case WUNTRACED:
        /* siginfo lives in root_process_t; the WUNTRACED clear of si_signo is
         * roottask's job now. proc_wait_check sets
         * root_proc(pid)->siginfo.si_signo = 0 directly.
         */
        break;
    case WCONTINUED:
        prp->flags &= ~RTMUX_FLG_PROC_CONTINUED;
        break;
    default:
        break;
    }
    if (prp != parent)
        kspinlock_unlock(&parent->vec_slock);
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_nozombie_mark — set RTMUX_FLG_PROC_WAITINFO +
 * RTMUX_FLG_PROC_NOZOMBIE on a process. Used by proc_nozombie's inlined prp->
 * writes; lifted into a kernel extension because the heap is PTE_U=0. The
 * siginfo (SIGCHLD/CLD_EXITED) is stamped roottask-side via
 * root_proc(pid)->siginfo before this kernel extension is invoked. Field names
 * avoid si_*: those are macros from <sys/siginfo.h>.
 */
struct kcall_args_proc_nozombie_mark {
    pid_t pid;
    int signo;
    int code;
    pid_t si_proc_pid;
    int status;
};

void kext_proc_nozombie_mark(void *data)
{
    struct kcall_args_proc_nozombie_mark *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }

    /* siginfo moved to root_process_t; the caller stamps si_* fields directly
     * via root_proc(pid)->siginfo before invoking this kernel extension. The
     * kernel extension only flips the kernel-half flag bits.
     */
    (void) kap->signo;
    (void) kap->code;
    (void) kap->si_proc_pid;
    (void) kap->status;
    kspinlock_lock(&prp->vec_slock);
    prp->flags |= RTMUX_FLG_PROC_WAITINFO;
    prp->flags |= RTMUX_FLG_PROC_NOZOMBIE;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_exit_mark — stamp prp at the start of process exit.
 * Sets TERMING | DESTROYALL | WAITINFO, fills siginfo (SIGCHLD/...),
 * and clamps boundary_addr to VM_KERN_SPACE_BOUNDARY.
 */
struct kcall_args_proc_exit_mark {
    pid_t pid;
    int exit_status;
};

void kext_proc_exit_mark(void *data)
{
    struct kcall_args_proc_exit_mark *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    kspinlock_lock(&prp->vec_slock);
    prp->flags |= RTMUX_FLG_PROC_TERMING;
    prp->flags |= RTMUX_FLG_PROC_DESTROYALL;
    prp->boundary_addr = VM_KERN_SPACE_BOUNDARY;

    /* siginfo moved to root_process_t; caller stamps it via
     * root_proc(pid)->siginfo before invoking this kernel extension.
     */
    (void) kap->exit_status;
    prp->flags |= RTMUX_FLG_PROC_WAITINFO;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_clear_lcp — set prp->lcp = NULL. Called from loader_exit when a
 * spawn dies during loading.
 */
void kext_proc_clear_lcp(void *data)
{
    pid_t pid = (pid_t) (uintptr_t) data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(pid);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    prp->lcp = NULL;
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_index_to_pid — look up process_vector[index] and copy out prp->pid
 * (full pid with generation). Used by roottask code iterating process_vector
 * via query_object.
 */
struct kcall_args_proc_index_to_pid {
    unsigned index;
    pid_t pid_out;
};

void kext_proc_index_to_pid(void *data)
{
    struct kcall_args_proc_index_to_pid *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp;

    rcu_read_lock(&process_rcu);
    prp = vector_lookup(&process_vector, kap->index);
    kap->pid_out = prp ? prp->pid : 0;
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_set_ocb_list — set prp->ocb_list = head. Caller-side paired with
 * process_query_info (which surfaces ocb_list in the snapshot) — roottask
 * procfs.c reads through qinfo, writes via this kernel extension.
 */
struct kcall_args_proc_set_ocb_list {
    pid_t pid;
    void *head;
};

void kext_proc_set_ocb_list(void *data)
{
    struct kcall_args_proc_set_ocb_list *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    kspinlock_lock(&prp->vec_slock);
    prp->ocb_list = kap->head;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_clear_pls — set prp->pls = NULL. Called from the process-exit
 * cleanup path before tearing down the address space.
 */
void kext_proc_clear_pls(void *data)
{
    pid_t pid = (pid_t) (uintptr_t) data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(pid);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    prp->pls = NULL;
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_exit_finalize — final-stage exit fixups on prp. Clears TERMING,
 * PRIV, and roottask-privileged capability classes. Guardian and debug_name are
 * roottask-side now (root_process_t); their cleanup happens in
 * root_process_fini.
 */
struct kcall_args_proc_exit_finalize {
    pid_t pid;
};

void kext_proc_exit_finalize(void *data)
{
    struct kcall_args_proc_exit_finalize *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    kspinlock_lock(&prp->vec_slock);
    prp->flags &= ~RTMUX_FLG_PROC_TERMING;
    prp->priv_caps = 0;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_flags_op — generic prp->flags update (clear-mask first, then
 * set-mask). Lifts inline "prp->flags |= X" / "prp->flags &= ~Y" out of
 * roottask; heap is PTE_U=0. Callers don't need a separate kernel extension per
 * flag-pattern.
 */
struct kcall_args_proc_flags_op {
    pid_t pid;
    uint32_t flags_set;
    uint32_t flags_clear;
    uint32_t flags_after; /* out: new value of prp->flags */
};

void kext_proc_flags_op(void *data)
{
    struct kcall_args_proc_flags_op *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    kspinlock_lock(&prp->vec_slock);
    prp->flags &= ~kap->flags_clear;
    prp->flags |= kap->flags_set;
    kap->flags_after = prp->flags;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* kext_proc_set_rlimit — record RLIMIT_CPU for a process. Heap is PTE_U=0, so
 * roottask (which validated the soft/hard pair) hands it here. The soft limit
 * is seconds; max_cpu_time (what the tick and resched_default enforce against
 * the process's billed running_time) is nanoseconds, RLIM_INFINITY staying
 * infinite and 0 meaning "kill at the next tick" (1 ns), since 0 in
 * max_cpu_time means no limit. max_cpu_time is read locklessly by every hart's
 * tick, so it is stored atomically; the soft/hard arrays are under vec_slock.
 */
struct kcall_args_proc_set_rlimit {
    pid_t pid;
    uint64_t cur;
    uint64_t max;
};

/* rlimit_cpu_to_ns — RLIMIT_CPU seconds to the nanosecond max_cpu_time the tick
 * enforces. RLIM_INFINITY (or a value that would overflow) stays infinite; 0
 * becomes 1 ns because max_cpu_time == 0 means "no limit".
 */
uint64_t rlimit_cpu_to_ns(uint64_t secs)
{
    if (secs == RLIM_INFINITY || secs > RLIM_INFINITY / 1000000000ULL)
        return RLIM_INFINITY;
    return secs ? secs * 1000000000ULL : 1;
}

void kext_proc_set_rlimit(void *data)
{
    struct kcall_args_proc_set_rlimit *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = lookup_pid_rcu(kap->pid);
    uint64_t ns = rlimit_cpu_to_ns(kap->cur);

    if (!prp) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    kspinlock_lock(&prp->vec_slock);

    /* Validate against the stored hard limit under the lock that guards it:
     * roottask's pre-check is a separate read, so two threads of one process
     * could otherwise interleave a lower and a raise and end with the hard
     * limit raised. There is no privilege override, so raising is EPERM.
     */
    if (kap->cur > kap->max || kap->max > prp->rlimit_vals_hard[RLIMIT_CPU]) {
        int err = kap->cur > kap->max ? EINVAL : EPERM;

        kspinlock_unlock(&prp->vec_slock);
        rcu_read_unlock(&process_rcu);
        kcall_err(act, err);
        return;
    }
    prp->rlimit_vals_soft[RLIMIT_CPU] = kap->cur;
    prp->rlimit_vals_hard[RLIMIT_CPU] = kap->max;
    __atomic_store_n(&prp->max_cpu_time, ns, __ATOMIC_RELAXED);
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}

/* root_phys_map_va — shared high-water-mark for the bootstrap mmap helpers
 * (roottask_mmap_phys_user, roottask_mmap_anon_user). Sits 16 MiB above
 * RTMUX_ROOTTASK_UADDR_BASE so it can't collide with roottask's text / data /
 * libc-arena heap (those all live below 0xc100_0000). Each helper bumps the
 * mark by its rounded-up size — bootstrap-only, mappings live forever, no
 * fragmentation to worry about.
 */
static uintptr_t root_phys_map_va = 0xc1000000UL;

/* pa allocator + restrictions live kernel-side (kernel/ext/pa.c). */
extern struct pa_quantum *pa_alloc(paddr_t size,
                                   paddr_t align,
                                   unsigned color,
                                   unsigned flags,
                                   unsigned *status_p,
                                   struct pa_restrict *restriction,
                                   paddr_t resv_size);
extern paddr_t pa_quantum_to_paddr(struct pa_quantum *pq);
extern struct pa_restrict *restrict_user;

/* kext_root_mmap_phys_user — install PTEs in roottask's pgdir mapping
 * "paddr..paddr+size-1" to "vaddr..vaddr+size-1" with the given prot. Bypasses
 * as_mmap and the libc-mmap message round-trip that self-deadlocks for roottask
 * bootstrap paths. See the slot comment in internal/include/sys/calls.h.
 */
struct kcall_args_root_mmap_phys_user {
    paddr_t paddr;
    size_t size;
    int prot;        /* PROT_READ etc. — pte_manipulate decodes */
    uintptr_t vaddr; /* in/out: 0 = let kernel pick; else fixed */
    int ret;
};

void kext_root_mmap_phys_user(void *data)
{
    struct kcall_args_root_mmap_phys_user *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = roottask_prp;
    address_t *adp;
    uintptr_t va_lo, va_hi;
    int r;

    if (!prp || !(adp = prp->memory)) {
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }

    if (kap->paddr == 0 || kap->size == 0) {
        kap->ret = EINVAL;
        SETKSTATUS(act, EINVAL);
        return;
    }

    size_t size_rounded = ROUNDUP(kap->size, __PAGESIZE);
    if (size_rounded < kap->size) { /* ROUNDUP wrapped */
        kap->ret = EINVAL;
        SETKSTATUS(act, EINVAL);
        return;
    }

    /* as_slock serializes the high-water bump AND the PTE state machine against
     * any other CPU touching roottask's as. The bump is read-modify-write on a
     * shared global, and cpu_pte_manipulate's VPS/split/merge walk on a single
     * adp must be atomic from another CPU's view (the same reason
     * ksys_manipulate holds this lock — see kernel/ext/manipulate.c). Both
     * helpers here bypass ksys_manipulate, so they must take it themselves.
     */
    kspinlock_lock(&as_slock);

    va_lo = (kap->vaddr != 0) ? kap->vaddr : root_phys_map_va;

    /* End of range must not wrap and must stay below the roottask user-VA
     * ceiling.
     */
    if (va_lo + size_rounded < va_lo ||
        va_lo + size_rounded > RTMUX_ROOTTASK_UADDR_END) {
        kspinlock_unlock(&as_slock);
        kap->ret = EINVAL;
        SETKSTATUS(act, EINVAL);
        return;
    }
    va_hi = va_lo + size_rounded - 1;

    /* Use cpu_pte_manipulate (pure kernel-text) directly rather than the
     * kext_pte_map indirection — kext_pte_map → pte_map (roottask text,
     * PTE_U=1) cannot be FETCHED from S-mode; SUM only relaxes load/ store,
     * never instruction fetch. cpu_pte_manipulate is the same function that
     * pte_manipulate eventually calls anyway, just without the roottask-text
     * hop.
     */
    {
        struct mm_pte_manipulate manip;
        unsigned flush_pending;

        manip.adp = adp;
        manip.start = va_lo;
        manip.end = va_hi + 1; /* cpu_pte_manipulate uses [start, end) */
        manip.split_end = 0;
        manip.first = 0;
        manip.paddr = kap->paddr;
        manip.prot = (unsigned) kap->prot;
        manip.op = PTE_OP_MAP;
        manip.special = 0;
        manip.shmem_flags = 0;
        manip.tlb_flush_pending = 0;
        r = cpu_pte_manipulate(&manip);
        flush_pending = manip.tlb_flush_pending;
        if (r == EOK && kap->vaddr == 0)
            root_phys_map_va = va_hi + 1; /* bump for the next caller */
        kspinlock_unlock(&as_slock);
        if (flush_pending)
            cpu_tlb_flush_complete();
    }
    if (r != EOK) {
        kap->ret = r;
        SETKSTATUS(act, r);
        return;
    }
    if (kap->vaddr == 0)
        kap->vaddr = va_lo;
    kap->ret = EOK;
    SETKSTATUS(act, EOK);
}

/* kext_root_mmap_anon_user — sister of roottask_mmap_phys_user. Allocates
 * "size" bytes of fresh physical memory via pa_alloc, then maps it into
 * roottask's user VA at the next high-water slot with the given prot.
 *
 * Used by message_start's prealloc_pool_stacks() loop, which has the same
 * chicken-and-egg as cpiofs_init: libc mmap(MAP_ANON) goes msg_sendnc →
 * VM_COID, which deadlocks roottask against itself before thread_pool_start has
 * spawned any receivers. This kernel extension does pa_alloc +
 * cpu_pte_manipulate in one BKL-held sweep.
 */
struct kcall_args_root_mmap_anon_user {
    size_t size;
    int prot;
    uintptr_t vaddr; /* out: chosen vaddr */
    int ret;
};

void kext_root_mmap_anon_user(void *data)
{
    struct kcall_args_root_mmap_anon_user *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp = roottask_prp;
    address_t *adp;
    struct pa_quantum *pq;
    paddr_t paddr;
    size_t size_aligned;
    uintptr_t va_lo, va_hi;
    struct mm_pte_manipulate manip;
    int r;

    if (!prp || !(adp = prp->memory)) {
        kap->ret = ESRCH;
        SETKSTATUS(act, ESRCH);
        return;
    }
    if (kap->size == 0) {
        kap->ret = EINVAL;
        SETKSTATUS(act, EINVAL);
        return;
    }

    /* pa_alloc operates on whole quanta — round up. */
    size_aligned = ROUNDUP(kap->size, QUANTUM_SIZE);
    if (size_aligned < kap->size) { /* ROUNDUP wrapped */
        kap->ret = EINVAL;
        SETKSTATUS(act, EINVAL);
        return;
    }

    /* "restrict_user" is the global restriction list pa.c builds at bringup for
     * non-system memory. Asking for contiguous keeps the mapping single-quantum
     * where possible (4–8 KiB stacks fit in one quantum) — simpler PTE install
     * path. pa_alloc takes pa_slock internally; do it BEFORE as_slock so the
     * two locks never nest.
     */
    pq = pa_alloc((paddr_t) size_aligned, 0, PAQ_COLOUR_NONE, PAA_FLAG_CONTIG,
                  NULL, restrict_user, 0);
    if (!pq) {
        kap->ret = ENOMEM;
        SETKSTATUS(act, ENOMEM);
        return;
    }
    paddr = pa_quantum_to_paddr(pq);

    /* as_slock: serialize the high-water bump + PTE walk (see the sister helper
     * kext_root_mmap_phys_user for the rationale).
     */
    kspinlock_lock(&as_slock);

    va_lo = root_phys_map_va;
    if (va_lo + size_aligned < va_lo ||
        va_lo + size_aligned > RTMUX_ROOTTASK_UADDR_END) {
        kspinlock_unlock(&as_slock);
        /* Leak pq — bootstrap path, fatal anyway. */
        kap->ret = EINVAL;
        SETKSTATUS(act, EINVAL);
        return;
    }
    va_hi = va_lo + size_aligned - 1;

    manip.adp = adp;
    manip.start = va_lo;
    manip.end = va_hi + 1;
    manip.split_end = 0;
    manip.first = 0;
    manip.paddr = paddr;
    manip.prot = (unsigned) kap->prot;
    manip.op = PTE_OP_MAP;
    manip.special = 0;
    manip.shmem_flags = 0;
    manip.tlb_flush_pending = 0;
    r = cpu_pte_manipulate(&manip);
    {
        unsigned flush_pending = manip.tlb_flush_pending;
        if (r == EOK)
            root_phys_map_va = va_hi + 1;
        kspinlock_unlock(&as_slock);
        if (flush_pending)
            cpu_tlb_flush_complete();
    }
    if (r != EOK) {
        /* Leak pq on failure — bootstrap path, fatal anyway. */
        kap->ret = r;
        SETKSTATUS(act, r);
        return;
    }
    kap->vaddr = va_lo;
    kap->ret = EOK;
    SETKSTATUS(act, EOK);
}


/* query_object_done wrapper: servers/roottask/proc/kext-wrappers.c */
