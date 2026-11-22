/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <sys/procmsg.h>


struct kcall_args_process_create {
    pid_t parent_pid;
    void *lcp;

    /* Non-zero when the child only carries the new image of an exec to its
     * parent, so it is no process the parent has created.
     */
    int exec;

    /* Output: the new process's adp pointer (== prp->memory). Roottask caches
     * it in root_process_t.adp so it can pass adp to kernel extensions (pte_*,
     * etc.) without a fresh lookup. Heap is PTE_U=0; the kernel writes this
     * slot before SETKSTATUS, U-mode roottask reads it from the kcall_args
     * struct sitting in its own user VA.
     */
    struct mm_as *adp_out;
    pid_t pid_out; /* same trick: copy out the new pid */
};

int rdecl process_pin(process_t *prp)
{
    unsigned count;

    if (!prp || prp->pid == 0)
        return ESRCH;

    count = __atomic_load_n(&prp->pin_count, __ATOMIC_RELAXED);
    for (;;) {
        if (count == UINT_MAX)
            return EAGAIN;
        if (__atomic_compare_exchange_n(&prp->pin_count, &count, count + 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            return EOK;
    }
}

void rdecl process_unpin(process_t *prp)
{
    unsigned old;

    old = __atomic_fetch_sub(&prp->pin_count, 1, __ATOMIC_RELEASE);
    if (old == 0)
        crash();
}

static void process_wait_unpinned(process_t *prp, unsigned held)
{
    while (__atomic_load_n(&prp->pin_count, __ATOMIC_ACQUIRE) != held)
        __asm__ __volatile__("" ::: "memory");
}

static void process_child_locks_lock(process_t *a, process_t *b)
{
    if (a == b) {
        kspinlock_lock(&a->vec_slock);
    } else if ((uintptr_t) a < (uintptr_t) b) {
        kspinlock_lock(&a->vec_slock);
        kspinlock_lock(&b->vec_slock);
    } else {
        kspinlock_lock(&b->vec_slock);
        kspinlock_lock(&a->vec_slock);
    }
}

static void process_child_locks_unlock(process_t *a, process_t *b)
{
    if (a == b) {
        kspinlock_unlock(&a->vec_slock);
    } else if ((uintptr_t) a < (uintptr_t) b) {
        kspinlock_unlock(&b->vec_slock);
        kspinlock_unlock(&a->vec_slock);
    } else {
        kspinlock_unlock(&a->vec_slock);
        kspinlock_unlock(&b->vec_slock);
    }
}

/* Detach every thread from target's address space before it is destroyed: the
 * threads of other processes that borrow it, from target's own borrower list,
 * then target's own threads. Neither step looks at any other process, so the
 * cost does not grow with the number of processes in the system.
 * Returns how many threads it visited.
 */
static unsigned process_clear_as_bindings(process_t *target)
{
    thread_t *thp;
    int tid;
    unsigned visited = thread_as_release_borrowers(target);

    kspinlock_lock(&target->vec_slock);
    for (tid = 0; tid < target->threads.nentries; ++tid) {
        if (VECP(thp, &target->threads, tid)) {
            thread_clear_as_prp_if(thp, target);
            visited++;
        }
    }
    kspinlock_unlock(&target->vec_slock);
    return visited;
}

/* kext_process_create — allocate and initialize a new process_t.
 *
 * Allocates a process soul, creates its address space, inherits the parent's
 * credentials / rlimits / dispatch queue, and links it into the parent's child
 * list. Copies the new pid and adp pointer back out via the kcall_args struct
 * so U-mode roottask need not dereference the kernel-allocated process_t.
 *
 * Locking:
 *   - process_vector_slock for vector_add/_rem
 *   - parent->vec_slock for the parent->child / num_processes chain
 *   - cred_slock around the cred_set call
 *   - limits_slock around lookup_limits + links bump
 *   - prp's own field init before vector_add is private
 * Parameters:
 *   data : struct kcall_args_process_create * (parent_pid, lcp; adp_out
 *          and pid_out are written on success)
 */
void kext_process_create(void *data)
{
    struct kcall_args_process_create *kap = data;
    pid_t pid;
    process_t *prp, *parent;
    thread_t *act = get_active_thread(get_cpunum());
    int status, i, gen;
    struct _cred_info info;

    if (!(parent = lookup_pid_rcu(kap->parent_pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(parent);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }

    /* The child of an exec is no new process for the caller, which gets the
     * image and keeps its pid: it must not count against RLIMIT_NPROC.
     */
    if (!kap->exec &&
        parent->num_processes >= parent->rlimit_vals_soft[RLIMIT_NPROC]) {
        process_unpin(parent);
        kcall_err(act, EAGAIN);
        return;
    }

    /* Check that we haven't run out of process vector entries */
    kspinlock_lock(&process_vector_slock);
    if ((process_vector.nentries - process_vector.nfree) >= PID_MASK - 1) {
        kspinlock_unlock(&process_vector_slock);
        process_unpin(parent);
        kcall_err(act, EAGAIN);
        return;
    }
    kspinlock_unlock(&process_vector_slock);

    /* Alloc a process entry. */
    if (!(prp = object_alloc(NULL, &process_souls))) {
        process_unpin(parent);
        kcall_err(act, ENOMEM);
        return;
    }

    /* Per-process vector lock — initialized before any vector_add on
     * prp->{threads,fdcons,chancons,...} so a concurrent observer sees a usable
     * lock. See struct comment in objects.h.
     */
    kspinlock_init(&prp->vec_slock, LOCK_CLASS_PROCVEC, "process_t.vec_slock");
    prp->pin_count = 0;
    prp->exec_thp = NULL;
    prp->as_borrowers = NULL;
    prp->as_borrow_closed = false;
    if (rtmux_cap_table_init(&prp->endpoint_caps, prp->endpoint_cap_slots,
                             RTMUX_PROCESS_ENDPOINT_CAP_SLOTS) != EOK) {
        object_free(NULL, &process_souls, prp);
        process_unpin(parent);
        kcall_err(act, ENOMEM);
        return;
    }

    if (kap->parent_pid) {
        prp->flags = RTMUX_FLG_PROC_LOADING | RTMUX_FLG_PROC_NOZOMBIE;
        prp->priv_caps = RTMUX_PRIV_CAP_ALL;
        prp->lcp = kap->lcp;
    }
    snap_time(&prp->start_time, 1);

    /* Allocate a vector for 1 thread but don't get a thread entry. */
    if (vector_add(&prp->threads, NULL, 1) == -1) {
        object_free(NULL, &process_souls, prp);
        process_unpin(parent);
        kcall_err(act, ENOMEM);
        return;
    }

    /* Add process to the process table vector. */
    kspinlock_lock(&process_vector_slock);
    if ((pid = vector_add(&process_vector, prp, 0)) == -1) {
        kspinlock_unlock(&process_vector_slock);
        vector_free(&prp->threads);
        object_free(NULL, &process_souls, prp);
        process_unpin(parent);
        kcall_err(act, ENOMEM);
        return;
    }
    gen = pid_unique; /* advanced under this lock by kext_process_destroy */
    kspinlock_unlock(&process_vector_slock);

    prp->boundary_addr = VM_KERN_SPACE_BOUNDARY;
    prp->pid = pid | gen;
    /* Call out to allow VM to initialize the address space. */
    if ((status = vm_ops->mcreate(prp)) != EOK) {
        kspinlock_lock(&process_vector_slock);
        vector_rem(&process_vector, PINDEX(pid));
        kspinlock_unlock(&process_vector_slock);
        vector_free(&prp->threads);
        object_free(NULL, &process_souls, prp);
        process_unpin(parent);
        kcall_err(act, status);
        return;
    }

    /* Inherit parents information */
    kspinlock_lock(&cred_slock);
    info = parent->cred->info;
    info.sgid = parent->cred->info.egid;
    info.suid = 0; /* The loader will set to euid after loading... */
    cred_set(&prp->cred, &info);
    kspinlock_unlock(&cred_slock);
    prp->seq = 1;

    /* inherit setrlimit/getrlimit settings. kext_proc_set_rlimit rewrites the
     * soft/hard pair under the parent's vec_slock, so copy under it too and a
     * concurrent setrlimit in another parent thread cannot be seen half done.
     * max_cpu_time is derived from the copied soft limit, not copied: a parent
     * whose limit already fired has max_cpu_time == 0 (disarmed) but still
     * reads back as set, and the child must inherit the limit, not the disarmed
     * state. The child's running_time starts at 0 (POSIX: CPU times are not
     * inherited, limits are).
     */
    kspinlock_lock(&parent->vec_slock);
    for (i = 0; i < RLIM_NLIMITS; i++) {
        prp->rlimit_vals_soft[i] = parent->rlimit_vals_soft[i];
        prp->rlimit_vals_hard[i] = parent->rlimit_vals_hard[i];
    }
    kspinlock_unlock(&parent->vec_slock);
    prp->max_cpu_time = rlimit_cpu_to_ns(prp->rlimit_vals_soft[RLIMIT_CPU]);

    /* stop core file generation if RLIMIT_CORE is 0 */
    if (prp->rlimit_vals_soft[RLIMIT_CORE] == 0) {
        prp->flags |= RTMUX_FLG_PROC_NOCOREDUMP;
    }

    /* Inherit default dispatch queue from creating thread. */
    prp->default_dpp = act->dpp;

    /* pgrp / session inheritance moved to roottask side: when the
     * process_create wrapper returns, roottask copies parent's pgrp into the
     * child's root_process_t slot and bumps session->links if the parent had a
     * session. See servers/roottask/proc/kcalls.c (process_create). Use the
     * euid already snapshotted under cred_slock above, not a fresh unlocked
     * read of parent->cred->info.euid: a concurrent cred_set on the parent can
     * replace or free parent->cred between the unlock above and here. The
     * snapshot also keeps the child's limits consistent with the child's
     * inherited credential.
     */
    kspinlock_lock(&limits_slock);
    if ((prp->limits = lookup_limits(info.euid)) ||
        (prp->limits = parent->limits)) {
        prp->limits->links++;
    }
    kspinlock_unlock(&limits_slock);

    /* Link the new process in as a child of its creator. */
    prp->child = NULL;
    __atomic_store_n(&prp->parent, parent, __ATOMIC_RELEASE);
    kspinlock_lock(&parent->vec_slock);
    prp->sibling = parent->child;
    parent->child = prp;
    ++parent->num_processes;
    kspinlock_unlock(&parent->vec_slock);
    _TRACE_PR_EMIT_CREATE(prp);

    /* Copy out the adp pointer and pid so roottask can populate root_process_t
     * without dereferencing the kernel-allocated process_t.
     */
    kap->adp_out = prp->memory;
    kap->pid_out = prp->pid;

    process_unpin(parent);
    SETKSTATUS(act, prp);
}


/* kext_process_destroyall — destroy all threads of a process.
 *
 * Looks up the process and tears down every thread via thread_destroyall, then
 * readies every stopped thread so each runs its KILLSELF and self-reaps. Only
 * the lead thread used to be readied: a second thread stopped by SIGSTOP stayed
 * stopped, never reaped itself, and kept a SIGKILLed process alive. Parameters:
 *   data : the target pid (cast from void *)
 */
void kext_process_destroyall(void *data)
{
    pid_t pid = (pid_t) (uintptr_t) data;
    thread_t *act = get_active_thread(get_cpunum());
    thread_t *thp;
    process_t *prp;
    int status;
    int unparked = 0;

    if (!(prp = lookup_pid_rcu(pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(prp);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }

    /* prp->vec_slock guards the prp->valid_thp probe + thread_destroyall (which
     * iterates prp->threads). ready() takes its own sched_slock; drop ours
     * around the call.
     */
    kspinlock_lock(&prp->vec_slock);
    if ((thp = prp->valid_thp)) {
        status = tthread_pin(thp);
        if (status != EOK) {
            kspinlock_unlock(&prp->vec_slock);
            process_unpin(prp);
            kcall_err(act, status);
            return;
        }
        thread_destroyall(thp);
        if (thp->state == STATE_STOPPED) {
            kspinlock_unlock(&prp->vec_slock);
            unparked |= exit_ready_stopped(thp);
            kspinlock_lock(&prp->vec_slock);
        }
        tthread_unpin(thp);
    }

    /* The other stopped threads too (exit_ready_stopped), also on a retry that
     * finds valid_thp already gone. ready() takes sched_slock, so drop
     * vec_slock around it and look each slot up again after. One not parked yet
     * makes the call fail with EAGAIN: the coredump path runs no exit pass that
     * would retry it, so process_destroy_all calls again.
     */
    for (unsigned tid = 0; tid < prp->threads.nentries; ++tid) {
        thread_t *t = vector_lookup2(&prp->threads, tid);

        if (!t || t == thp || t->state != STATE_STOPPED ||
            tthread_pin(t) != EOK)
            continue;
        kspinlock_unlock(&prp->vec_slock);
        unparked |= exit_ready_stopped(t);
        kspinlock_lock(&prp->vec_slock);
        tthread_unpin(t);
    }
    kspinlock_unlock(&prp->vec_slock);

    process_unpin(prp);
    if (unparked) {
        kcall_err(act, EAGAIN);
        return;
    }
    SETKSTATUS(act, 0);
}


/* kext_process_destroy — free a dead process and its resources.
 *
 * Reparents any surviving children onto the kernel process, removes the
 * process_vector slot, drains in-flight RCU readers, frees the per- process
 * vectors / credential / limits, unlinks from the parent, and frees the process
 * soul. No-op if the process is still being queried (a query_object is in
 * progress).
 *
 * Locking:
 *   - process_vector_slock for entry lookup + later vector_rem
 *   - parent->vec_slock for the parent's child-chain unlink
 *   - prp's own sub-vector tear-downs are post-rcu_sched_synchronize, after
 *     which prp is private to this CPU
 * Parameters:
 *   data : the target pid (cast from void *)
 */
void kext_process_destroy(void *data)
{
    process_t *prp, *parent, *child;
    pid_t pid = (pid_t) (uintptr_t) data;
    thread_t *act = get_active_thread(get_cpunum());

    /* Match the whole pid, generation included, and claim the teardown in the
     * same lock window. Roottask can send a second destroy for a pid
     * (proc_terminate and the WAITDONE reap in proc_exit may both fire): once
     * the index is reused, an index-only lookup finds the new live process and
     * tears it down with its address space still attached, and two destroys
     * that both pass the lookup would free the process twice.
     *
     * A process with a query_object() in flight cannot be freed under the
     * reader. Say so (EAGAIN) and let the caller retry: success here used to be
     * returned without destroying anything, and roottask then released its half
     * of the process, leaving a live kernel process nobody could look up or
     * exit.
     */
    {
        int err = 0;

        kspinlock_lock(&process_vector_slock);

        /* Open-coded: the RCU-sched gate allows no plain pid-lookup helper
         * call, and this lookup holds process_vector_slock instead.
         */
        if (!(prp = vector_lookup(&process_vector, PINDEX(pid))) ||
            prp->pid != pid || prp->destroying)
            err = ESRCH;
        else if (prp->querying || prp->reparenting)
            err = EAGAIN;
        else
            prp->destroying = true;
        kspinlock_unlock(&process_vector_slock);
        if (err) {
            kcall_err(act, err);
            return;
        }
    }

    _TRACE_PR_EMIT_DESTROY(prp, pid);

    /* retarget all children to PROC */
    for (;;) {
        process_t *proc = kernel_threads_prp;
        struct sigevent ev;
        int send_event = 0;

        process_child_locks_lock(prp, proc);
        if (!(child = prp->child)) {
            process_child_locks_unlock(prp, proc);
            break;
        }
        prp->child = child->sibling;
        if (proc_hooks->proc_threads_destroyed &&
            (((child->flags & (RTMUX_FLG_PROC_LOADING | RTMUX_FLG_PROC_TERMING |
                               RTMUX_FLG_PROC_ZOMBIE)) ==
              RTMUX_FLG_PROC_ZOMBIE) ||
             (child->flags & RTMUX_FLG_PROC_NOZOMBIE))) {
            __atomic_fetch_and(
                &child->flags,
                ~(unsigned) (RTMUX_FLG_PROC_ZOMBIE | RTMUX_FLG_PROC_WAITINFO),
                __ATOMIC_RELAXED);
            send_event = 1;
        }

        /* The flags go before the new parent is published, so that a lockless
         * reader (kext_proc_query) that sees the new parent cannot still see
         * the ZOMBIE this takes on itself the reap of, and destroy the child
         * under the callback below.
         */
        __atomic_store_n(&child->parent, proc, __ATOMIC_RELEASE);
        child->sibling = proc->child;
        proc->child = child;
        --prp->num_processes;
        ++proc->num_processes;
        process_child_locks_unlock(prp, proc);

        if (send_event) {
            (*proc_hooks->proc_threads_destroyed)(child, &ev);
            sigevent_proc(&ev);
        }
        /* Loop might take a while - give intr queue a chance to drain. */
        KEXT_PREEMPT(act);
    }

    kspinlock_lock(&process_vector_slock);

    /* Advance the generation before the index is freed, under the same lock
     * kext_process_create reads it under, so a process created at this index
     * afterwards can never carry the old pid. Roottask tags its per-pid slot
     * with the pid, and a reused pid let the old generation's slot release
     * clear the new generation's slot. The mask keeps pids positive.
     */
    pid_unique = (pid_unique + (PID_MASK + 1)) & INT_MAX;
    vector_rem(&process_vector, PINDEX(pid));
    kspinlock_unlock(&process_vector_slock);

    /* RCU drain: at this point no NEW reader can find prp via process_vector
     * (the slot is gone). But a concurrent BKL-less kcall on another CPU may
     * still hold a prp pointer it captured BEFORE our vector_rem. Wait here for
     * every in-flight process_rcu read section to exit before recycling memory.
     */
    rcu_sched_synchronize();
    process_wait_unpinned(prp, 0);

    prp->pid = 0; /* clear so assertion below confirms removal */

    /* Remove the thread vector */
    if (prp->threads.nentries && prp->threads.nentries == prp->threads.nfree)
        vector_free(&prp->threads);

    /* Remove the timer vector */
    if (prp->timers.nentries && prp->timers.nentries == prp->timers.nfree)
        vector_free(&prp->timers);

    /* Remove the fd vector */
    if (prp->fdcons.nentries && prp->fdcons.nentries == prp->fdcons.nfree)
        vector_free(&prp->fdcons);

    /* Remove the channel vector */
    if (prp->chancons.nentries && prp->chancons.nentries == prp->chancons.nfree)
        vector_free(&prp->chancons);

    /* Unlink the credential. cred_set mutates the shared per-euid refcount
     * (credential_t.links) and, on the last drop, unlinks and frees the entry
     * from credential_list. That shared state is serialized by cred_slock at
     * every other cred_set site, so hold it here too; an unlocked drop races a
     * concurrent locked cred_set on another hart.
     */
    if (prp->cred) {
        kspinlock_lock(&cred_slock);
        cred_set(&prp->cred, NULL);
        kspinlock_unlock(&cred_slock);
    }

    /* Session refcount/free moved to roottask: root_process_t.session is
     * decremented
     * + freed in the proc_terminate path before the kernel gets here. Nothing
     * for the kernel to do.
     */

    /* Release this process's reference to the shared per-euid limits object.
     * links is bumped under limits_slock at attach (kext_process_create), and
     * limits_list is guarded by the same lock, so decrement, test, and unlink
     * under it too. A plain -- here raced a concurrent create's locked ++ on
     * another hart and lost an update, underflowing links (the ~0U guard) or
     * freeing a limits object a live process still references. Capture the
     * post-decrement value so the underflow guard does not read a freed object,
     * and free after releasing the lock: once unlinked from limits_list under
     * the lock, no concurrent lookup_limits can find it.
     */
    if (prp->limits) {
        unsigned after;

        kspinlock_lock(&limits_slock);
        after = --prp->limits->links;
        if (after == 0)
            LINK1_REM(limits_list, prp->limits, limits_t);
        kspinlock_unlock(&limits_slock);
        if (after == ~0U)
            crash();
        if (after == 0)
            object_free(NULL, &limits_souls, prp->limits);
    }
    if (prp->pid)
        crash();
    if (prp->cred)
        crash();
    if (prp->alarm)
        crash();
    if (prp->nfds)
        crash();
    if (prp->chancons.vector) {
        vector_free(&prp->chancons);
    }
    if (prp->fdcons.vector) {
        vector_free(&prp->fdcons);
    }
    if (prp->threads.vector) {
        vector_free(&prp->threads);
    }
    if (prp->timers.vector) {
        vector_free(
            &prp->timers); /* force free to avoid leak, but investigate */
    }
    if (prp->memory) {
        /* Name the process: the crash line alone does not say which teardown
         * path skipped the address-space shutdown.
         */
        kprintf(
            "process destroy: pid %d flags %#x still has an address space\n",
            pid, (unsigned) prp->flags);
        crash();
    }
    if (prp->join_queue)
        crash();
    /*  if(prp->session)            crash(); */
    if (prp->debugger)
        crash();

    /* prp->lock was a roottask-owned mux pointer; moved to root_process_t.
     * Roottask now asserts the slot is clean at root_process_fini time.
     */
    if (prp->num_active_threads)
        crash();

    /* rsrc_list and conf_table moved to root_process_t (roottask side); their
     * cleanup-asserted-NULL checks now happen in roottask during exit.
     */

    /* Unlink from parent under parent->vec_slock */
    for (;;) {
        parent = __atomic_load_n(&prp->parent, __ATOMIC_ACQUIRE);
        kspinlock_lock(&parent->vec_slock);

        /* The parent's own teardown moves its children to the kernel process
         * under the old parent's lock, so a parent still named after the lock
         * is taken stays the parent. That move happens at most once, so this
         * retries at most once; kext_reparent cannot run here, because the
         * destroying claim above keeps it out.
         */
        if (__atomic_load_n(&prp->parent, __ATOMIC_ACQUIRE) == parent)
            break;
        kspinlock_unlock(&parent->vec_slock);
    }
    --parent->num_processes;
    if (prp == parent->child) {
        parent->child = prp->sibling;
    } else {
        process_t *sib;

        for (sib = parent->child; sib; sib = sib->sibling) {
            if (sib->sibling == prp) {
                sib->sibling = prp->sibling;
                break;
            }
        }
    }
    /* The lock taken above, not a fresh read of prp->parent. */
    kspinlock_unlock(&parent->vec_slock);

    /* Revoke every process capability naming this dying process across all
     * process tables before its soul is recycled, mirroring the connection
     * soul-free path: a PROCESS cap minted over prp (see rtmux_cap_alloc_typed)
     * and IPC-transferred to another process would otherwise outlive the soul
     * and resolve to a reused process_t. prp is already out of process_vector
     * (vector_rem above), so the fan-out scans only the other live tables. This
     * finds nothing until a process cap is first minted; it exists so that mint
     * can never open a use-after-free window.
     */
    cap_revoke_object_all_tables(prp);
    object_free(NULL, &process_souls, prp);
    SETKSTATUS(act, 1);
}

struct kcall_args_process_startup {
    pid_t pid;
    int start;
    uint64_t priv_caps;
};

/* kext_process_startup — finish loading a process and enter U-mode.
 *
 * When kap->start is set, elects the lead thread, applies the loaded
 * credentials, sets its entry IP/SP, tears down roottask's view of the child's
 * address space, and transitions the thread to U-mode.
 *
 * Returns the loader context pointer (lcp). Parameters:
 *   data : struct kcall_args_process_startup * (pid, start)
 */
void kext_process_startup(void *data)
{
    struct kcall_args_process_startup *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp;
    struct _cred_info info;
    void *lcp;
    int status;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(prp);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }

    if ((prp->flags & RTMUX_FLG_PROC_LOADING) == 0) {
        process_unpin(prp);
        kcall_err(act, ESRCH);
        return;
    }

    lcp = prp->lcp;

    if (kap->start) {
        uintptr_t start_ip;
        thread_t *thp;

        /* The loader pre-publishes initial_ip on the child's prp before sending
         * SPAWN_DONE. Read it directly; roottask callbacks are gone.
         */
        start_ip = prp->initial_ip;
        if (start_ip == 0) {
            process_unpin(prp);
            kcall_err(act, ENOEXEC);
            return;
        }

        /* Distributed authority must be a subset of the starter's own. The
         * spawn committer (roottask) computes the child's final priv_caps from
         * the bootstrap manifest; the kernel refuses to widen beyond what the
         * committer itself holds. Checked before any start side effect so a
         * rejected grant leaves the child untouched. roottask holds
         * RTMUX_PRIV_CAP_ALL, so a well-formed manifest grant always passes.
         */
        if (kap->priv_caps & ~act->process->priv_caps) {
            process_unpin(prp);
            kcall_err(act, EPERM);
            return;
        }

        /* Locking:
         *   - cred_slock around the cred read+write
         *   - prp->vec_slock around the valid_thp election walk
         */

        kspinlock_lock(&cred_slock);
        info = prp->cred->info;
        info.suid = info.euid;
        info.sgid = info.egid;
        cred_set(&prp->cred, &info);
        kspinlock_unlock(&cred_slock);
        kspinlock_lock(&prp->vec_slock);
        thp = prp->valid_thp;

        /* Fallback: prp->valid_thp may be NULL on the spawn-done path even
         * though a single live loader thread sits in prp->threads. The loader
         * thread, on its way through thread_specret/cpu_thread_waaa, can
         * transit a state where the thread-destroy bookkeeping has cleared
         * valid_thp before the spawn-done message is delivered to roottask.
         * Walk prp->threads and pick the first non-dead one — there is exactly
         * one for a freshly-spawned process at this point.
         */
        if (!thp) {
            for (unsigned i = 0; i < prp->threads.nentries; i++) {
                thread_t *t = vector_lookup(&prp->threads, i);
                if (t && t->state != STATE_DEAD) {
                    thp = t;
                    prp->valid_thp = t;
                    break;
                }
            }
        }
        if (!thp) {
            kspinlock_unlock(&prp->vec_slock);
            process_unpin(prp);
            kcall_err(act, ESRCH);
            return;
        }
        status = tthread_pin(thp);
        if (status != EOK) {
            kspinlock_unlock(&prp->vec_slock);
            process_unpin(prp);
            kcall_err(act, status);
            return;
        }
        kspinlock_unlock(&prp->vec_slock);
        /* Don't allow anything to change the IP after this point. */
        atomic_or(&thp->flags, RTMUX_FLG_THR_KCALL_ERR_SET);
        if (!(prp->flags & RTMUX_FLG_PROC_FORKED)) {
            SETKIP_FUNC(thp, start_ip);
            SETKSP(thp, prp->initial_sp);
        }
        /* The loader thread has been running in the CHILD's pgdir all along (no
         * satp switch needed here), with roottask's image mapped at
         * RTMUX_ROOTTASK_UADDR_BASE via mm_roottask_view_install in
         * lifecycle.c's loader path. Tear that view down now so the child
         * enters U-mode fully isolated from roottask. After this point the only
         * thing in the child's pgdir at L2[3] is zero — any user mmap into that
         * range is now unblocked, and any stray pointer deref into roottask
         * text/data faults.
         *
         * (thp->as_prp = prp is already true from thread_create's loader_path
         * branch; the explicit assignment below is defensive.)
         */
        {
            extern void mm_roottask_view_remove(process_t *);
            mm_roottask_view_remove(prp);
        }
        if (thread_set_as_prp(thp, prp) != EOK)
            crash();
        cpu_thread_enter_umode(thp);
        _TRACE_NOARGS(thp);

        /* @@@ This should be examined in more detail later!!! */
        prp->boundary_addr = WITHIN_BOUNDARY(prp->initial_sp, prp->initial_sp,
                                             user_boundary_addr)
                                 ? user_boundary_addr
                                 : VM_KERN_SPACE_BOUNDARY;
        prp->flags &= ~RTMUX_FLG_PROC_LOADING;
        prp->priv_caps = kap->priv_caps;
        prp->lcp = 0;
        _TRACE_PR_EMIT_CREATE_NAME(prp);
        tthread_unpin(thp);
    }

    /* Marker — guards the SETKSTATUS write on the boundary back to roottask.
     * TODO: no slock needed once kcall-return ordering is explicit.
     */
    process_unpin(prp);
    SETKSTATUS(act, lcp);
}


/* kext_proc_set_spawn — publish a child's entry point and spawn flags.
 *
 * Writes initial_ip / initial_sp on the child's prp and applies the NOZOMBIE
 * rule of the spawn flags. The kernel heap is mapped PTE_U=0, so
 * U-mode roottask cannot touch prp->initial_ip / prp->flags directly.
 *
 * The (possibly-augmented) caller-side spawn flags are returned via
 * spawn_flags_out so the loader can keep its lcp->ctx.flags in sync.
 */
struct kcall_args_proc_set_spawn {
    pid_t pid;
    uintptr_t initial_ip;
    uintptr_t initial_sp;
    unsigned spawn_flags_in;
    unsigned spawn_flags_out;
};

void kext_proc_set_spawn(void *data)
{
    struct kcall_args_proc_set_spawn *kap = data;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp;
    unsigned flags;

    if (!(prp = lookup_pid_rcu(kap->pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }

    /* prp->vec_slock (PROCVEC class) guards the prp scalar field writes (flags
     * / initial_ip / initial_sp) as a coarse per-prp lock; prp is in
     * PROC_LOADING.
     */

    kspinlock_lock(&prp->vec_slock);
    flags = kap->spawn_flags_in;

    /* By default kext_process_create leaves NOZOMBIE set (so a child dying
     * inside roottask never zombies); the loader clears it for normal spawns so
     * the parent's waitpid() works. An exec's child is only a shell that
     * carries the new image to the caller and then dies; it stays NOZOMBIE, so
     * that nothing ever waits for it. The caller's own disposition is its own
     * flag, which the exec does not touch.
     */
    if (!(flags & (SPAWN_NOZOMBIE | SPAWN_EXEC)))
        prp->flags &= ~RTMUX_FLG_PROC_NOZOMBIE;

    prp->initial_ip = kap->initial_ip;
    prp->initial_sp = kap->initial_sp;

    kap->spawn_flags_out = flags;
    kspinlock_unlock(&prp->vec_slock);
    rcu_read_unlock(&process_rcu);
    SETKSTATUS(act, EOK);
}


static void process_as_release_harts(process_t *prp);
static unsigned process_clear_as_bindings(process_t *target);

/* exec() replaces the image of the process that calls it, and keeps the
 * process: its pid, descriptors, parent and children stay. Roottask loads the
 * new image into a fresh child, the loader shell, and holds that child's loader
 * thread in a reply; the calling thread is also held in a reply, to the spawn
 * request. The swap then goes in three kernel steps, with roottask's own
 * bookkeeping between:
 *
 *   BEGIN    kill every thread of the caller but the one that called exec;
 *   (roottask drops what the old image owned and shuts its address space down
 *    with the ordinary process_shutdown)
 *   QUIESCE  take the shell's address space off every hart that still has it
 *            loaded, so that it can change owners;
 *   COMMIT   give that address space and its entry point to the caller, and
 *            send the surviving thread to the new image's first instruction
 *            when roottask replies to it.
 *
 * The surviving thread is the exec caller: it is already a thread of the
 * process, and holding it through the swap keeps the thread vector from ever
 * emptying, so the process is never reported as having lost its last thread.
 * After the commit the shell has no threads left bar the loader thread, and
 * roottask retires it as a process that died while loading.
 */
struct kcall_args_process_exec {
    unsigned op;
    pid_t ppid;         /* BEGIN, COMMIT: the process that called exec */
    int lrcvid;         /* BEGIN, COMMIT: rcvid of the shell's loader thread */
    int crcvid;         /* BEGIN, COMMIT: rcvid of the exec caller */
    pid_t shell_pid;    /* QUIESCE: the shell */
    uint64_t priv_caps; /* COMMIT: authority the new image is started with */
};

/* Resolve the four parties of an exec from the two rcvids roottask holds: the
 * loader thread of the shell (reply-blocked in _PROC_SPAWN_EXEC) and the thread
 * that called exec (reply-blocked in the spawn request). Leaves the rcvid RCU
 * sections open, which the caller closes with lookup_rcvid_release() when it is
 * going to wait, and pins nothing.
 *
 * Returns 0, ENOERROR when lookup_rcvid already set the error, or an errno.
 */
static int exec_parties(thread_t *act,
                        const struct kcall_args_process_exec *kap,
                        thread_t **loaderp,
                        thread_t **callerp,
                        process_t **shellp,
                        process_t **prpp)
{
    thread_t *lthp, *cthp;
    connect_t *cop, *ccop;
    process_t *shell, *prp;

    if (!(cop = lookup_rcvid(NULL, kap->lrcvid, &lthp)))
        return ENOERROR;
    if (lthp->state != STATE_REPLY || lthp->blocked_on != cop)
        return ESRCH;

    shell = lthp->process;
    prp = shell->parent;
    if (!prp || prp == shell || prp->pid != kap->ppid ||
        !(shell->flags & RTMUX_FLG_PROC_LOADING))
        return ESRCH;

    /* The caller's rcvid names a connection in roottask's channel table and a
     * thread slot of the process that owns the connection.
     */
    ccop = vector_lookup(&act->process->chancons, MCINDEX(kap->crcvid));
    if (!ccop || ccop->type != TYPE_CONNECTION || ccop->process != prp)
        return ESRCH;
    cthp = vector_lookup(&prp->threads, MTINDEX(kap->crcvid));
    if (!cthp || cthp->process != prp || cthp->state != STATE_REPLY ||
        cthp->blocked_on != ccop ||
        (cthp->internal_flags & RTMUX_ITF_SPECRET_PENDING))
        return ESRCH;

    *loaderp = lthp;
    *callerp = cthp;
    *shellp = shell;
    *prpp = prp;
    return EOK;
}

/* EXEC_BEGIN: take every thread of the process but the caller down.
 *
 * Returns EAGAIN while one is still on its way out; roottask calls again, so
 * that a thread held by a message-delivery claim or a stack release gets to run
 * on this hart. A process with an exec already under way (another thread called
 * exec first, and has since been killed) or one that is exiting refuses.
 */
static void exec_begin(thread_t *act, const struct kcall_args_process_exec *kap)
{
    thread_t *lthp, *cthp;
    process_t *shell, *prp;
    unsigned nthreads;
    int rc;

    if ((rc = exec_parties(act, kap, &lthp, &cthp, &shell, &prp)) != EOK) {
        if (rc != ENOERROR)
            kcall_err(act, rc);
        return;
    }

    /* Pin both, then let go of the rcvid sections: killing a thread waits for
     * every RCU reader, this call's own among them.
     */
    if ((rc = process_pin(prp)) != EOK) {
        kcall_err(act, rc);
        return;
    }
    if ((rc = tthread_pin(cthp)) != EOK) {
        process_unpin(prp);
        kcall_err(act, rc);
        return;
    }
    kspinlock_lock(&prp->vec_slock);
    if (prp->flags & (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL |
                      RTMUX_FLG_PROC_LOADING | RTMUX_FLG_PROC_ZOMBIE))
        rc = ESRCH;
    else if (prp->exec_thp && prp->exec_thp != cthp)
        rc = EBUSY;
    else
        prp->exec_thp = cthp;
    kspinlock_unlock(&prp->vec_slock);
    if (rc != EOK) {
        tthread_unpin(cthp);
        process_unpin(prp);
        kcall_err(act, rc);
        return;
    }

    lookup_rcvid_release();

    exit_destroy_threads(prp, cthp);
    if ((int) KSTATUS(act) == EOK) {
        kspinlock_lock(&prp->vec_slock);
        nthreads = prp->threads.nentries - prp->threads.nfree;
        kspinlock_unlock(&prp->vec_slock);
        if (nthreads != 1)
            kcall_err(act, EAGAIN);
    }
    tthread_unpin(cthp);
    process_unpin(prp);
}

/* EXEC_QUIESCE: detach the shell's address space from every thread and hart.
 *
 * The loader thread ran in it, and any hart that ran the loader thread may
 * still have its page tables loaded. Once this returns nothing runs in it, so
 * the commit can hand it to the caller; a hart that loaded it later would walk
 * page tables owned by a process it does not know.
 */
static void exec_quiesce(thread_t *act,
                         const struct kcall_args_process_exec *kap)
{
    process_t *shell;
    int status;

    if (!(shell = lookup_pid_rcu(kap->shell_pid))) {
        rcu_read_unlock(&process_rcu);
        kcall_err(act, ESRCH);
        return;
    }
    status = process_pin(shell);
    rcu_read_unlock(&process_rcu);
    if (status != EOK) {
        kcall_err(act, status);
        return;
    }

    if (!shell->memory || !(shell->flags & RTMUX_FLG_PROC_LOADING)) {
        process_unpin(shell);
        kcall_err(act, ESRCH);
        return;
    }
    (void) process_clear_as_bindings(shell);
    process_wait_unpinned(shell, 1);
    process_as_release_harts(shell);
    process_unpin(shell);
    SETKSTATUS(act, EOK);
}

/* EXEC_COMMIT: the caller takes over the shell's address space and entry point.
 *
 * Everything is checked before anything changes, under both processes'
 * vec_slock: the caller's own address space is already shut down, the caller is
 * the one thread left and still waiting in its spawn reply, and neither process
 * is exiting. The pointer moves in one critical section, so no reader sees the
 * address space in both processes or in neither once the call is past the
 * checks. The caller's thread then gets the new image's TLS, entry and stack,
 * and its registers (FP included) are cleared; roottask's reply is what lets it
 * run, in the new address space, at the entry point.
 */
static void exec_commit(thread_t *act,
                        const struct kcall_args_process_exec *kap)
{
    thread_t *lthp, *cthp;
    process_t *shell, *prp;
    struct _cred_info info;
    uintptr_t start_ip, start_sp;
    int rc;

    if ((rc = exec_parties(act, kap, &lthp, &cthp, &shell, &prp)) != EOK) {
        if (rc != ENOERROR)
            kcall_err(act, rc);
        return;
    }
    if (kap->priv_caps & ~act->process->priv_caps) {
        kcall_err(act, EPERM);
        return;
    }
    if (cthp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        kcall_err(act, EAGAIN);
        return;
    }

    /* The thread-local storage the loader thread built holds the identity of
     * the loader thread: the shell's pid, its own thread id and the owner word
     * its mutexes carry. It is about to be the caller's, so make it the
     * caller's: written in the shell's address space, which this hart takes up
     * for the copy, while the shell still owns it and before anything changes.
     */
    {
        unsigned cpu = get_cpunum();
        process_t *saved_as_prp = pcpu_array[cpu].pc_as_prp;
        struct _thread_local_storage *tls = lthp->un.lcl.tls;
        int ids[3] = {
            (int) prp->pid,
            (int) (cthp->tid + 1),
            (int) SYNC_OWNER_BITS(prp->pid, cthp->tid),
        };

        /* __pid, __tid and __owner follow one another. */
        _Static_assert(
            offsetof(struct _thread_local_storage, __tid) ==
                    offsetof(struct _thread_local_storage, __pid) +
                        sizeof(int) &&
                offsetof(struct _thread_local_storage, __owner) ==
                    offsetof(struct _thread_local_storage, __tid) + sizeof(int),
            "TLS identity fields are not contiguous");
        vm_ops->as(shell, &pcpu_array[cpu].pc_as_prp);
        rc = xfer_memcpy(&tls->__pid, ids, sizeof ids);
        vm_ops->as(saved_as_prp, &pcpu_array[cpu].pc_as_prp);
        if (rc != 0) {
            kcall_err(act, EFAULT);
            return;
        }
    }

    /* Take a reference to the final credentials before moving the image.
     * Allocation failure leaves the caller and shell owning their originals. */
    credential_t *new_cred = NULL;
    kspinlock_lock(&cred_slock);
    info = shell->cred->info;
    info.suid = info.euid;
    info.sgid = info.egid;
    rc = cred_set(&new_cred, &info);
    kspinlock_unlock(&cred_slock);
    if (rc != EOK) {
        kcall_err(act, ENOMEM);
        return;
    }

    /* PROCVEC lock instances of two processes, in pointer order. */
    process_child_locks_lock(prp, shell);
    if (prp->exec_thp != cthp || prp->memory || !shell->memory ||
        prp->num_active_threads != 1 ||
        prp->threads.nentries - prp->threads.nfree != 1 ||
        (prp->flags & (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_DESTROYALL)) ||
        !shell->initial_ip || !shell->as_borrow_closed) {
        process_child_locks_unlock(prp, shell);
        kspinlock_lock(&cred_slock);
        cred_set(&new_cred, NULL);
        kspinlock_unlock(&cred_slock);
        kcall_err(act, ESRCH);
        return;
    }

    start_ip = shell->initial_ip;
    start_sp = shell->initial_sp;
    prp->memory = shell->memory;
    shell->memory = NULL;
    prp->boundary_addr = WITHIN_BOUNDARY(start_sp, start_sp, user_boundary_addr)
                             ? user_boundary_addr
                             : VM_KERN_SPACE_BOUNDARY;
    prp->initial_ip = start_ip;
    prp->initial_sp = start_sp;
    prp->base_addr = shell->base_addr;
    prp->pls = NULL;
    prp->canstub = NULL;
    prp->priv_caps = kap->priv_caps;
    prp->valid_thp = cthp;
    prp->exec_thp = NULL;
    process_child_locks_unlock(prp, shell);

    kspinlock_lock(&cred_slock);
    cred_dirty(prp);
    cred_set(&prp->cred, NULL);
    prp->cred = new_cred;
    kspinlock_unlock(&cred_slock);

    thread_as_reopen_borrowers(prp);
    if (thread_set_as_prp(cthp, prp) != EOK)
        crash();

    /* The new main thread's thread-local storage, set up by the loader thread
     * on its own entry. Its old stack, if the kernel allocated one, went with
     * the old address space.
     */
    cthp->un.lcl.tls = lthp->un.lcl.tls;
    cthp->un.lcl.stackaddr = lthp->un.lcl.stackaddr;
    cthp->un.lcl.stacksize = lthp->un.lcl.stacksize;
    atomic_clr(&cthp->flags, RTMUX_FLG_THR_ALLOCED_STACK);

    /* Don't allow anything to change the IP after this point. */
    atomic_or(&cthp->flags, RTMUX_FLG_THR_KCALL_ERR_SET);
    SETKIP_FUNC(cthp, start_ip);
    SETKSP(cthp, start_sp);
    {
        extern void mm_roottask_view_remove(process_t *);

        /* The shell's page tables carry the view of roottask's image that its
         * loader thread ran on; the new image must not see it.
         */
        mm_roottask_view_remove(prp);
    }
    cpu_thread_enter_umode(cthp);
    SETKSTATUS(act, EOK);
}

/* kext_process_exec — one step of the image swap of an exec (see above).
 * Parameters:
 *   data : struct kcall_args_process_exec * (op and the arguments it uses)
 */
void kext_process_exec(void *data)
{
    const struct kcall_args_process_exec *kap = data;
    thread_t *act = get_active_thread(get_cpunum());

    switch (kap->op) {
    case EXEC_BEGIN:
        exec_begin(act, kap);
        break;
    case EXEC_QUIESCE:
        exec_quiesce(act, kap);
        break;
    case EXEC_COMMIT:
        exec_commit(act, kap);
        break;
    default:
        kcall_err(act, EINVAL);
        break;
    }
}

void rdecl set_safe_as(unsigned cpu)
{
    /* The address space that pcpu_array[cpu].pc_as_prp is pointing at is going
     * away. Point things at someplace safe.
     *
     * Only swap SATP when the active thread on this hart has NO as (i.e. it's a
     * kernel thread like the idle thread). For user threads, their next
     * kernel_exit will swap SATP to their own as via vm_ops->as() — set_safe_as
     * would be redundant.
     *
     * Use the roottask process (ROOTTASK_PID=1). kernel_threads_prp->memory is
     * NULL — its only role is to hold kernel-resident thread_t structures.
     * Calling as_switch with a NULL-memory target is a no-op (it early-returns
     * at line 32 of as.c), which would leave pcpu_array[cpu].pc_as_prp pointing
     * at the dying process and loop the IPI_TLB_SAFE retry in
     * kext_process_shutdown forever.
     */
    thread_t *act = get_active_thread(cpu);
    if (act && act->as_prp) {
        /* A non-idle thread with an as is running here; decline to swap and let
         * its next kernel_exit drop the dying as.
         */
        return;
    }

    if (!roottask_prp || !roottask_prp->memory) {
        /* Roottask not up yet — boot path can't reach this code, but be
         * defensive: nothing safer to switch to. The cost of leaving
         * pcpu_array[cpu].pc_as_prp alone here is that the caller's spin won't
         * terminate; that's better than a NULL deref.
         */
        return;
    }
    vm_ops->as(roottask_prp, &pcpu_array[cpu].pc_as_prp);
}



struct kcall_args_process_shutdown {
    process_t *prp;
    int exec;
};

#if defined(CONFIG_INSTRUMENTATION)
/* Address-space teardown cost (section 18 companion to the irqoff report).
 * "unbind" spans process_clear_as_bindings through process_wait_unpinned, the
 * part whose cost used to grow with the number of processes in the system, and
 * "visited" counts the threads process_clear_as_bindings looked at; "total"
 * spans the whole address-space branch of kext_process_shutdown through
 * mdestroy. Updated lock-free by any hart, read racily by the report.
 */
static uint64_t pshut_count, pshut_unbind_sum, pshut_unbind_max;
static uint64_t pshut_visit_sum, pshut_visit_max;
static uint64_t pshut_total_sum, pshut_total_max;

static void pshut_max(uint64_t *slot, uint64_t cyc)
{
    uint64_t cur = __atomic_load_n(slot, __ATOMIC_RELAXED);

    while (cyc > cur &&
           !__atomic_compare_exchange_n(slot, &cur, cyc, 0, __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED))
        ;
}

static void pshut_record(uint64_t t0,
                         uint64_t t1,
                         uint64_t t2,
                         unsigned visited)
{
    __atomic_fetch_add(&pshut_count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&pshut_visit_sum, visited, __ATOMIC_RELAXED);
    pshut_max(&pshut_visit_max, visited);
    __atomic_fetch_add(&pshut_unbind_sum, t1 - t0, __ATOMIC_RELAXED);
    __atomic_fetch_add(&pshut_total_sum, t2 - t0, __ATOMIC_RELAXED);
    pshut_max(&pshut_unbind_max, t1 - t0);
    pshut_max(&pshut_total_max, t2 - t0);
}

static inline uint64_t pshut_ticks(void)
{
    return cpu_clock_ticks();
}

/* Cycles to ns without forming cyc * 1e9, which wraps past about 1.8e10 cycles:
 * whole seconds first, then the remainder, which is below cps.
 */
static uint64_t pshut_ns(uint64_t cyc, uint64_t cps)
{
    return cyc / cps * 1000000000ULL + cyc % cps * 1000000000ULL / cps;
}

/* Print the teardown cost in ns. Called throttled from clock_handler next to
 * nonpreempt_report(), outside any lock.
 */
void process_shutdown_report(void)
{
    uint64_t cps = qtimeptr->cycles_per_sec ? qtimeptr->cycles_per_sec : 1;
    uint64_t n = __atomic_load_n(&pshut_count, __ATOMIC_RELAXED);
    uint64_t visit_sum = __atomic_load_n(&pshut_visit_sum, __ATOMIC_RELAXED);
    uint64_t visit_max = __atomic_load_n(&pshut_visit_max, __ATOMIC_RELAXED);
    uint64_t unbind_sum = __atomic_load_n(&pshut_unbind_sum, __ATOMIC_RELAXED);
    uint64_t unbind_max = __atomic_load_n(&pshut_unbind_max, __ATOMIC_RELAXED);
    uint64_t total_sum = __atomic_load_n(&pshut_total_sum, __ATOMIC_RELAXED);
    uint64_t total_max = __atomic_load_n(&pshut_total_max, __ATOMIC_RELAXED);

    static uint64_t last_n;

    if (__atomic_exchange_n(&last_n, n, __ATOMIC_RELAXED) == n)
        return;
    kprintf(
        "pshut: count=%llu visited_avg=%llu visited_max=%llu "
        "unbind_avg_ns=%llu unbind_max_ns=%llu "
        "total_avg_ns=%llu total_max_ns=%llu\n",
        (unsigned long long) n, (unsigned long long) (visit_sum / n),
        (unsigned long long) visit_max,
        (unsigned long long) pshut_ns(unbind_sum / n, cps),
        (unsigned long long) pshut_ns(unbind_max, cps),
        (unsigned long long) pshut_ns(total_sum / n, cps),
        (unsigned long long) pshut_ns(total_max, cps));
}
#else
static inline uint64_t pshut_ticks(void)
{
    return 0;
}

static inline void pshut_record(uint64_t t0,
                                uint64_t t1,
                                uint64_t t2,
                                unsigned visited)
{
    (void) t0;
    (void) t1;
    (void) t2;
    (void) visited;
}
#endif

/* process_as_release_harts — move every hart off prp's address space.
 *
 * The caller has already unbound every thread from the address space
 * (process_clear_as_bindings), so no hart takes it up again; this waits out the
 * ones that still have it loaded. Used before the address space is freed, and
 * before an exec hands the loader's one to another process. Parameters:
 *   prp : process whose address space is going away or changing hands
 */
static void process_as_release_harts(process_t *prp)
{
    /* Force every remote hart still pointing at the dying as to swap to the
     * roottask as before we free prp->memory. Once the dying pgdir PA is
     * returned to pa_alloc, the next caller (e.g. loader_elf mapping an ELF
     * segment) will write module bytes over it, and a remote hart with a
     * stale SATP will walk those bytes as PTEs on the very next TLB miss —
     * infinite double-fault in trap.S trying to save registers to a
     * now-unmapped kernel stack.
     *
     * IPI_TLB_SAFE's handler calls set_safe_as(cpu) on the remote hart,
     * atomically updating both pcpu_array[i].pc_as_prp AND the SATP CSR via
     * vm_ops->as(). We loop until no remote hart still matches: per remote,
     * send IPI, wait for ipicmds[] bit to clear, then drain any in-flight
     * message delivery so we don't pull the as out from under an xfer. The
     * ack wait used to live in the U-mode wrapper, which worked when
     * roottask shared the kernel address space (S-mode); a U-mode roottask
     * cannot read ipicmds[] / get_active_thread() so the wait must happen
     * here in the kernel body.
     */
#if CONFIG_PROCESSORS_MAX > 1
    {
        for (;;) {
            bool matched = false;
            uint64_t online = cpu_online_snapshot();
            for (int _hi = 0; _hi < num_processors; ++_hi) {
                unsigned i = (unsigned) _hi;
                process_t *prp0;

                if (!cpu_in_mask(online, i))
                    continue;

                /* On RISC-V 64-bit, pointer loads are atomic. */
                prp0 = *((process_t *volatile *) &pcpu_array[i].pc_as_prp);
                if (prp != prp0)
                    continue;
                matched = true;

                if (i == get_cpunum()) {
                    set_safe_as(i);
                    continue;
                }

                SENDIPI(i, IPI_TLB_SAFE);
                /* Wait for the remote hart to process the IPI.
                 *
                 * Briefly enable interrupts each iteration so a sibling's
                 * IPI_CRASH can deliver and yank us into
                 * crash_quiesce_self() instead of leaving us 100% stuck
                 * here. Same pattern as the acquire_kernel BKL spin and
                 * intrevent_flush.
                 *
                 * Re-issue the IPI every ~1024 spins: a single initial
                 * SENDIPI can be lost to a sip.SSIP-vs- sie.SSIE race (the
                 * remote hart's SSIE briefly cleared during a transition,
                 * so the SBI-injected SSIP never fires a trap). Re-sending
                 * periodically converts that into a self-healing wait at
                 * negligible cost: the remote hart processes whichever IPI
                 * lands first, the rest are no-op TAS clears of 0.
                 */
                {
                    unsigned spins = 0;
                    while (atomic_load_explicit(&pcpu_array[i].pc_ipicmds,
                                                memory_order_acquire) &
                           IPI_TLB_SAFE) {
                        MEM_BARRIER_RW();
                        interrupt_enable();
                        interrupt_disable();
                        if ((++spins & 0x3ff) == 0) {
                            /* Re-fire the SBI ECALL — atomic_or 0 is a
                             * no-op on the command bits but send_ipi
                             * unconditionally calls sbi_send_ipi, which
                             * sets fresh sip.SSIP on the target.
                             */
                            SENDIPI(i, 0);
                        }
                    }
                }

                /* Drain in-flight message delivery on the remote hart so we
                 * don't free the as under an xfer.
                 *
                 * RTMUX_ITF_MSG_DELIVERY is in "internal_flags", NOT
                 * "flags". The two enum spaces overlap at bit 0
                 * (RTMUX_ITF_MSG_DELIVERY=0x01 /
                 * RTMUX_FLG_THR_KILLSELF=0x01), so testing the wrong field
                 * would turn this into a "spin while the remote thread has
                 * KILLSELF set" loop — which never terminates once the
                 * process is exiting.
                 */
                for (;;) {
                    thread_t *thp;
                    MEM_BARRIER_RW();
                    thp = get_active_thread(i);
                    if (!thp || !(thp->internal_flags & RTMUX_ITF_MSG_DELIVERY))
                        break;
                    /* SIE-toggle to admit IPI_CRASH from a sibling. */
                    interrupt_enable();
                    interrupt_disable();
                }
            }
            if (!matched)
                break;
        }
    }
#else
    if (prp == pcpu_array[get_cpunum()].pc_as_prp)
        set_safe_as(get_cpunum());
#endif
}

/* kext_process_shutdown — tear down a process's address space.
 *
 * Releases every thread bound to prp's address space (prp's borrower list, then
 * prp->threads) and NULLs its thp->as_prp, then IPI- coordinates remote harts
 * off the dying as via IPI_TLB_SAFE + ack-wait + MSG_DELIVERY drain before
 * calling vm_ops->mdestroy(prp). If the process has no address space, instead
 * notifies the parent and destroys the calling thread.
 *
 * prp->vec_slock guards the prp->threads walk. The IPI_TLB_SAFE round- trip is
 * load-bearing and runs without a slock. Parameters:
 *   data : struct kcall_args_process_shutdown * (prp, exec); !prp
 *          means the calling thread's own process
 */
void kext_process_shutdown(void *data)
{
    struct kcall_args_process_shutdown *args = data;
    process_t *prp = args->prp;
    thread_t *act = get_active_thread(get_cpunum());
    /* tSigTable removed — signals eliminated */

    if (!prp) {
        prp = act->process;
    }

    if (prp->memory) {
        uint64_t pshut_t0 = pshut_ticks();

        /* Make sure no native or foreign thread retains this as before it is
         * removed.
         */
        unsigned pshut_visited = process_clear_as_bindings(prp);

        /* The borrower list is closed and drained now, so only transient pins
         * (kcalls holding a promoted lookup) remain to drain.
         */
        if (prp->as_borrowers || !prp->as_borrow_closed)
            crash();
        process_wait_unpinned(prp, 0);
        uint64_t pshut_t1 = pshut_ticks();

        process_as_release_harts(prp);
        vm_ops->mdestroy(prp);
        pshut_record(pshut_t0, pshut_t1, pshut_ticks(), pshut_visited);

        if (prp->memory || pcpu_array[get_cpunum()].pc_as_prp == prp) {
            crash();
        }
        SETKSTATUS(act, 0);
        return;
    }

    if (prp == act->process) {
        if (!args->exec) {
            /* Notify parent that spawned process terminated (replaces SIGCHLD)
             */
            if (!(prp->flags & RTMUX_FLG_PROC_NOZOMBIE)) {
                notification_notify_proc_terminated(prp->parent, prp);
            }
            /* siginfo lives in root_process_t; nothing to stamp here. */
        }
        pcpu_array[get_cpunum()].pc_active_prp = kernel_threads_prp;
        kspinlock_lock(&prp->vec_slock);
        thread_destroyall(act);
        kspinlock_unlock(&prp->vec_slock);
    } else {
        /* Caller used to receive prp->siginfo.si_status as the kernel extension
         * return; with siginfo gone from prp, return 0 (success). No kernel
         * caller currently relies on the status — the only reader was
         * kext_process_destroyall, which discards it.
         */
        SETKSTATUS(act, 0);
    }
}


struct kcall_args_thread_tls {
    struct _thread_local_storage *tls;
    uintptr_t stackaddr;
    size_t stacksize;
    size_t guardsize;
    uintptr_t esp;
};

/* kext_thread_tls — set up thread-local storage for the active thread.
 *
 * When a stack pointer is supplied, places a copy of the current TLS block at
 * the top of the new stack (bounds-checked against a wrapping or kernel-half
 * esp) and records it; then records stack address, guard size, and stack size
 * on the thread. Parameters:
 *   data : struct kcall_args_thread_tls * (tls, stackaddr, stacksize,
 *          guardsize, esp); tls is set to the installed TLS pointer
 */
void kext_thread_tls(void *data)
{
    struct kcall_args_thread_tls *kap = data;
    thread_t *act = get_active_thread(get_cpunum());

    if (kap->esp) {
        struct _thread_local_storage *tsp;
        uintptr_t aligned_esp = kap->esp & ~(STACK_ALIGNMENT - 1);
        size_t tls_sz = sizeof *act->un.lcl.tls;
        void *olderr;

        /* Bounds-check: kap->esp comes straight from a U-mode pthread_create()
         * argument and is not validated by the libc wrapper. If aligned_esp <
         * tls_sz, the subtraction below wraps around to the top of the address
         * space (a very-negative pointer), the struct copy faults at a
         * sign-extended high VA (e.g., 0xffff_ffff_ffff_ffc8), and depending on
         * the trap classification it either SIGSEGVs the user or escalates into
         * the kernel. A well-behaved caller has aligned_esp comfortably above
         * tls_sz; reject anything else with an early SETKSTATUS so the libc
         * wrapper turns it into an EFAULT/EINVAL return to userspace.
         *
         * Also reject any kap->esp that points into the kernel half — a U-mode
         * caller has no business handing the kernel extension a kernel-virtual
         * stack pointer.
         */
        if (aligned_esp < tls_sz ||
            aligned_esp >= (uintptr_t) 0xffffffc000000000UL) {
            kcall_err(act, EFAULT);
            return;
        }

        tsp = (struct _thread_local_storage *) (aligned_esp - tls_sz);

        /* The destination is the new thread's stack, which the caller chose and
         * can leave unmapped: copy through xfer_memcpy so a fault returns
         * EFAULT instead of faulting the kernel, leaving the old TLS pointer
         * installed.
         */
        if (xfer_memcpy(tsp, act->un.lcl.tls, tls_sz) != 0) {
            kcall_err(act, EFAULT);
            return;
        }
        /* Read the old __errptr through the fault-safe copier too. */
        if (xfer_memcpy(&olderr, &act->un.lcl.tls->__errptr, sizeof olderr) !=
            0) {
            kcall_err(act, EFAULT);
            return;
        }
        if (olderr == &act->un.lcl.tls->__errval) {
            void *errptr = &tsp->__errval;

            if (xfer_memcpy(&tsp->__errptr, &errptr, sizeof errptr) != 0) {
                kcall_err(act, EFAULT);
                return;
            }
        }
        kap->tls = tsp;
    }

    {
        struct _thread_local_storage *newtls =
            kap->tls ? kap->tls : act->un.lcl.tls;
        uint8_t *stackbase = (uint8_t *) kap->stackaddr + kap->guardsize;

        /* Publish the new TLS pointer only once the last write to it worked. */
        if (xfer_memcpy(&newtls->__stackaddr, &stackbase, sizeof stackbase) !=
            0) {
            kcall_err(act, EFAULT);
            return;
        }
        act->un.lcl.tls = newtls;
    }
    act->un.lcl.stackaddr = (void *) kap->stackaddr;
    act->un.lcl.stacksize = kap->stacksize;

    SETKSTATUS(act, kap->tls);
}

/* thread_tls wrapper: servers/roottask/proc/kext-wrappers.c */


struct reparent_args {
    pid_t pid;
};

/* kext_reparent — reparent a process onto kernel_threads_prp.
 *
 * Unlinks the process from its current parent's child list and links it into
 * kernel_threads_prp's child list, adjusting num_processes on both. Parameters:
 *   data : struct reparent_args * (pid, generation included)
 *
 * The caller names the process by pid, not by pointer: roottask can destroy a
 * dying process without holding its slot lock, so a pointer it looked up
 * earlier may already be freed. The lookup pins the process the way an
 * in-flight query does, so kext_process_destroy answers EAGAIN, and the caller
 * of destroy retries, until the move is done. Fails with ESRCH when the pid is
 * stale or its teardown has already been claimed.
 */
void kext_reparent(void *data)
{
    struct reparent_args *args = data;
    pid_t pid = args->pid;
    thread_t *act = get_active_thread(get_cpunum());
    process_t *prp;
    process_t *old_parent;
    process_t *new_parent = kernel_threads_prp;
    process_t **pp;

    kspinlock_lock(&process_vector_slock);
    prp = vector_lookup(&process_vector, PINDEX(pid));
    if (!prp || prp->pid != pid || prp->destroying || prp->reparenting) {
        kspinlock_unlock(&process_vector_slock);
        kcall_err(act, ESRCH);
        return;
    }
    prp->reparenting = true;
    kspinlock_unlock(&process_vector_slock);

    /* Only destroy changes the parent besides this call, and the pin above
     * keeps destroy out, so the parent read here stays the parent.
     */
    old_parent = __atomic_load_n(&prp->parent, __ATOMIC_ACQUIRE);
    process_child_locks_lock(old_parent, new_parent);

    /* Unlink from old parent's child list */
    for (pp = &old_parent->child; *pp; pp = &(*pp)->sibling) {
        if (*pp == prp) {
            *pp = prp->sibling;
            break;
        }
    }
    --old_parent->num_processes;

    /* Link into new parent's child list */
    __atomic_store_n(&prp->parent, new_parent, __ATOMIC_RELEASE);
    prp->sibling = new_parent->child;
    new_parent->child = prp;
    ++new_parent->num_processes;

    process_child_locks_unlock(old_parent, new_parent);

    kspinlock_lock(&process_vector_slock);
    prp->reparenting = false;
    kspinlock_unlock(&process_vector_slock);
    SETKSTATUS(act, EOK);
}


struct kext_signal_args {
    channel_t *chp;
    int signo;
    pid_t sender_pid;
};

/* kext_signal_deliver — deliver a signal notification to a channel.
 *
 * Packs the signal number and sender pid into the 64-bit notification value
 * (signo in the low 32 bits, sender pid in the high 32) and delivers it.
 * Parameters:
 *   arg : struct kext_signal_args * (chp, signo, sender_pid)
 */
void kext_signal_deliver(void *arg)
{
    struct kext_signal_args *kap = arg;
    /* signo in the low 32 bits, sender pid in the high 32 (0 if none). */
    uint32_t spid = kap->sender_pid > 0 ? (uint32_t) kap->sender_pid : 0;
    uint64_t value = ((uint64_t) spid << 32) | (uint32_t) kap->signo;

    /* Terminal-generated signals carry the dispatcher's receive priority
     * through to the process. Ordinary signals leave the receiver at base.
     */
    unsigned prio = RTMUX_PRIO_SERVER;
    if (!spid && (kap->signo == SIGINT || kap->signo == SIGQUIT ||
                  kap->signo == SIGTSTP)) {
        thread_t *act = get_active_thread(get_cpunum());
        unsigned limit = min(kopt.priv_prio - 1U, RTMUX_PRIO_DEVICE_IST);
        if (limit)
            prio = min((unsigned) act->priority, limit) |
                   _NOTIFICATION_PRIO_SIGNAL;
    }
    notification_deliver(kap->chp, prio, _NOTIFICATION_CODE_SIGNAL,
                         (uintptr_t) value, 0, 0);
}


/* proc_threads_destroyed — kernel-resident termination event composer, wired
 * into proc_hooks->proc_threads_destroyed by init_vm_ops().
 *
 * Called from lifecycle.c:thread_destroy when the last live thread in a process
 * is removed. The job is purely to compose a sigevent that tells proc service
 * "pid X is now zero-thread; please run termination cleanup". The caller
 * (lifecycle.c) calls sigevent_proc(&ev) immediately after, which delivers the
 * notification to ROOTTASK_COID.
 *
 * The PROC_CODE_TERM constant matches servers/roottask/proc/private.h:
 *   PROC_CODE_TERM = SI_MINAVAIL + 1
 * SI_MINAVAIL is in <sys/siginfo.h> (already pulled in via signal.h).
 */
#ifndef PROC_CODE_TERM_KERN
#define PROC_CODE_TERM_KERN (SI_MINAVAIL + 1)
#endif

void proc_threads_destroyed(process_t *prp, struct sigevent *evp)
{
    evp->sigev_notify = SIGEV_NOTIFICATION;
    evp->sigev_coid = ROOTTASK_COID; /* == ROOTTASK_COID */
    evp->sigev_value.sival_int = prp->pid;
    evp->sigev_priority = prp->terming_priority ? prp->terming_priority : 10;
    evp->sigev_code = PROC_CODE_TERM_KERN;
}
