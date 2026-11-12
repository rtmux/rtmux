/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <user-slice.h>

static inline void thread_join_relax(void)
{
    __asm__ volatile(".word 0x0100000f" ::: "memory");
}

static void thread_join_wait_ready(thread_t *thp)
{
    while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
        thread_join_relax();
    cpu_smp_mb();
}

static inline unsigned thread_destroy_reserve_token(void)
{
    return THREAD_DESTROY_CLAIM_RESERVED_BASE + get_cpunum();
}

static int thread_reserve_destroy(thread_t *thp)
{
    unsigned expected = 0;

    return __atomic_compare_exchange_n(&thp->destroy_claim, &expected,
                                       thread_destroy_reserve_token(), 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* Create a thread in a process (__thread_create kcall).
 *
 * Looks up the target process (default: the caller's own), enforces the
 * thread-count rlimit, and creates a blocked thread via thread_create from the
 * supplied attributes and entry/argument. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (target pid, attr, func, arg)
 * Return value: the thread_create result; ESRCH/EPERM/EINVAL on failure.
 */
int kdecl ksys_thread_create(thread_t *act,
                             struct kcall_args_thread_create *kap)
{
    process_t *prp;
    struct sigevent event;
    struct _thread_attr attr_copy;
    int ret;
    bool prp_pinned;

    /* User attribute probing runs before any target process pin is taken.
     * Foreign-pid lookup then uses process_rcu only long enough to pin the
     * target process, and thread_create() runs after the RCU read section is
     * closed.
     *
     * Residual: the prp->num_active_threads check is best-effort (TOCTOU
     * against another concurrent thread_create on the same prp). The
     * authoritative rlimit enforcement belongs inside thread_create under
     * prp->vec_slock; that's a lifecycle.c audit item, not in this kcall's
     * scope.
     */

    /* Probe caller attributes before pinning a target process. */
    if (kap->attr) {
        /* Copy in the caller's address space; the new thread may run in a
         * different one, so thread_specret reads this copy, never the pointer.
         */
        if (user_slice_get(act, &attr_copy, kap->attr, sizeof attr_copy))
            return EFAULT;
    }

    /* Verify the process to create the thread in exists. */
    prp_pinned = false;
    if (kap->pid == act->process->pid) {
        prp = act->process;
    } else if (kap->pid) {
        prp = lookup_pid_rcu(kap->pid);
        if (prp && process_pin(prp) == EOK)
            prp_pinned = true;
        else
            prp = NULL;
        rcu_sched_read_unlock();
        if (!prp)
            return ESRCH;
    } else {
        prp = act->process;
    }

    /* Creating a thread in another process is a process-management authority.
     * Two independent ways to hold it: the coarse RTMUX_PRIV_CAP_PROCESS class
     * (roottask and manifest-granted servers, which may target any process), or
     * a per-object process capability over this specific target carrying
     * RTMUX_CAP_PROCESS_MAKE_THREAD (a parent that received its child's cap
     * from rtmux_spawn_cap). rtmux_cap_has_object_typed scans the caller's own
     * endpoint table for a live PROCESS cap naming prp with that right. The
     * type predicate is required, not decorative: this is a security gate, so
     * it must not admit a cap of another class that happens to name the same
     * address. A started child has priv_caps 0 and holds no such cap. A child
     * holds every cap transiently while RTMUX_FLG_PROC_LOADING; exclude that
     * window so a loading child cannot inject a cross-process thread.
     */
    if (prp != act->process &&
        ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
         (!(act->process->priv_caps & RTMUX_PRIV_CAP_PROCESS) &&
          !rtmux_cap_has_object_typed(&act->process->endpoint_caps, prp,
                                      RTMUX_CAP_PROCESS_MAKE_THREAD,
                                      RTMUX_CAP_TYPE_PROCESS)))) {
        ret = EPERM;
        goto out;
    }
    /* check that we're not exceed any imposed limits */
    if (prp->num_active_threads >= prp->rlimit_vals_soft[RLIMIT_NTHR]) {
        ret = EINVAL; /* is this the right error code? */
        goto out;
    }
    event.sigev_notify_attributes = NULL;
    event.sigev_notify_function = kap->func;
    event.sigev_value.sival_ptr = kap->arg;


    ret = thread_create(act, prp, &event, THREAD_CREATE_BLOCK_FLAG,
                        kap->attr ? &attr_copy : NULL);
out:
    if (prp_pinned)
        process_unpin(prp);
    return ret;
}

/* "create_sigchld" no longer lives in the kernel: it filled prp->siginfo on
 * thread exit so waitpid() readers could see the SIGCHLD payload. siginfo has
 * moved to root_process_t (POSIX policy isn't the kernel's job); roottask fills
 * siginfo itself on the proc_exit / proc_terminate path, reading running_time /
 * system_time via process_query_info to compute utime/stime.
 */

/* Destroy a thread (__thread_destroy kcall).
 *
 * Destroys the target thread (or the last thread, falling through to the
 * destroy-all path when only zombies remain). __thread_destroy(-1) is rejected
 * — process exit goes through the _PROC_EXIT message. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (tid, exit status, terminating priority)
 * Return value: EOK / ENOERROR on success; ESRCH if the tid is absent.
 */
int kdecl ksys_thread_destroy(thread_t *act,
                              struct kcall_args_thread_destroy *kap)
{
    thread_t *thp;
    process_t *prp = act->process;

    /* prp->vec_slock around the threads vector_lookup, held across the
     * thread_destroy call so the captured thp can't be vector_rem'd by a
     * concurrent destroyer between our lookup and thread_destroy's own
     * vector_rem. The all-threads path takes the slock briefly around the
     * snapshot of nentries/nfree; thread_destroyall walks the vector internally
     * and acquires its own slock per iteration.
     */

    /* __thread_destroy(-1) is no longer valid. Process exit goes through
     * _PROC_EXIT message to proc service. Catch any stale callers.
     */
    if (kap->tid == -1) {
        kprintf("FATAL: __thread_destroy(-1) called by pid=%d tid=%d\n",
                prp->pid, act->tid);
        crash_halt(
            "__thread_destroy(-1) is invalid — use _exit() / _PROC_EXIT");
    }


    if ((int) kap->priority > 0 && (int) kap->priority <= NUM_PRI - 1) {
        prp->terming_priority = (uint8_t) kap->priority;
    } else {
        prp->terming_priority = act->priority;
    }

    /* Verify the target thread exists. */
    if (kap->tid != -1) {
        kspinlock_lock(&prp->vec_slock);
        thp = kap->tid ? vector_lookup(&prp->threads, (unsigned) (kap->tid - 1))
                       : act;
        if (!thp) {
            kspinlock_unlock(&prp->vec_slock);
            return ESRCH;
        }

        /* A single thread exit may result in the last thread going away in
         * which case the process gets a zero exit status.
         */
        if (prp->num_active_threads > 1 ||
            (prp->threads.nentries - 1 == prp->threads.nfree)) {
            thp->status = kap->status;
            kspinlock_unlock(&prp->vec_slock);
            thread_destroy(thp);
            return EOK;
        }
        kspinlock_unlock(&prp->vec_slock);

        /* To get here, this must be the last thread and we have zombies which
         * need to be purged. This is handled by the multi-code below.
         */
    }
    /* We are going to take out all threads. */

    /* We force the caller to kill all its sibling threads. */
    thread_destroyall(act);
    return ENOERROR;
}


/* Destroy all sibling threads then the caller (thread_destroy_all kcall).
 *
 * Walks the process's thread vector killing every thread but the caller
 * (preserving roottask's per-CPU idle threads), then destroys the calling
 * thread itself. Parameters:
 *   act : the calling (active) thread
 *   kap : unused
 * Return value: ENOERROR.
 */
int kdecl ksys_thread_destroyall(thread_t *act, struct kcall_args_null *kap)
{
    int tid;
    int nentries;
    thread_t *thp;
    process_t *prp = act->process;

    /* prp->vec_slock around the per-iteration slot lookup; released before
     * per-thread writes + thread_destroy call (rank-inversion otherwise —
     * thread_destroy acquires vec_slock internally for vector_rem). Snapshot
     * nentries under the slock so growth from a concurrent thread_create
     * doesn't extend our walk past the original count (any newly-created thread
     * will be killed by the second pass during process exit, or by
     * thread_destroy(act) below).
     *
     * Per-thread writes (thp->flags, thp->status) and the thread_destroy call
     * run with thp captured but unprotected — a thread_t UAF surface
     * (RCU-retire-on-thread_t is the cohesive fix).
     */


    /* If ONLYME is set, thp->status was must be set before setting KILLSELF. If
     * there is only one thread left, cleanup any zombies (for race condition in
     * stack de-allocation)
     */
    if ((act->flags & RTMUX_FLG_THR_ONLYME) == 0 ||
        prp->num_active_threads == 1) {
        int killed = 0;

        kspinlock_lock(&prp->vec_slock);
        nentries = prp->threads.nentries;
        kspinlock_unlock(&prp->vec_slock);

        for (tid = 0; tid < nentries; ++tid) {
            kspinlock_lock(&prp->vec_slock);
            thp = VECP2(thp, &prp->threads, tid);
            kspinlock_unlock(&prp->vec_slock);

            if (thp && thp != act &&
                !(prp->pid == ROOTTASK_PID && thp->tid < num_processors)) {
                atomic_or(&thp->flags,
                          (RTMUX_FLG_THR_DETACHED | RTMUX_FLG_THR_KILLSELF));
                thp->status = 0;
                thread_destroy(thp);
                atomic_clr(&act->flags, RTMUX_FLG_THR_ONLYME);
                killed++;
                KSYS_PREEMPT(act, ENOERROR);
            }
        }
        atomic_or(&act->flags, RTMUX_FLG_THR_DETACHED);
        act->status = 0;
    }

    thread_destroy(act);
    return ENOERROR;
}


/* Detach a thread (thread_detach kcall).
 *
 * Marks the target thread detached, waking any pending joiner with EINVAL, and
 * reaps it immediately if it has already exited. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->tid is the thread to detach (0 = self)
 * Return value: EOK on success; ESRCH/EINVAL on failure.
 */
int kdecl ksys_thread_detach(thread_t *act,
                             struct kcall_args_thread_detach *kap)
{
    thread_t *thp;
    thread_t *joiner = NULL;
    int wake_joiner = 0;
    process_t *prp = act->process;
    unsigned claim_spins = 0;
    int target_dead;

    /* prp->vec_slock protects vector_lookup2, DETACHED publication, and the
     * joiner handoff. The wakeup and final reap run after unlock because they
     * take scheduler/vector locks internally.
     */

    /* Verify the specified thread exists. */
retry:
    kspinlock_lock(&prp->vec_slock);
    thp = kap->tid ? vector_lookup2(&prp->threads, (unsigned) (kap->tid - 1))
                   : act;
    if (!thp) {
        kspinlock_unlock(&prp->vec_slock);
        return ESRCH;
    }
    if (kap->tid && thp->tid != (uint32_t) (kap->tid - 1)) {
        kspinlock_unlock(&prp->vec_slock);
        return ESRCH;
    }

    if (thp->flags & RTMUX_FLG_THR_DETACHED) {
        kspinlock_unlock(&prp->vec_slock);
        return EINVAL;
    }
    target_dead = thp->state == STATE_DEAD;
    if (target_dead && !thread_reserve_destroy(thp)) {
        kspinlock_unlock(&prp->vec_slock);
        if (++claim_spins > 1000000) {
            kprintf("FATAL: thread_detach destroy claim stuck tid=%d\n",
                    kap->tid);
            crash();
        }
        thread_join_relax();
        goto retry;
    }
    joiner = thp->join;
    if (joiner && tthread_pin(joiner) != EOK)
        crash();
    atomic_or(&thp->flags, RTMUX_FLG_THR_DETACHED);
    if (target_dead)
        thp->status = 0;
    kspinlock_unlock(&prp->vec_slock);

    /* If anyone was waiting to join on me wake them up with an error */
    if (joiner) {
        thread_join_wait_ready(joiner);
        kspinlock_lock(&prp->vec_slock);
        if (thp->join == joiner) {
            thp->join = NULL;
            wake_joiner = 1;
        }
        kspinlock_unlock(&prp->vec_slock);
        if (wake_joiner)
            force_ready(joiner, EINVAL);
        tthread_unpin(joiner);
    }

    if (target_dead)
        thread_destroy(thp);

    return EOK;
}


/* Wait for a thread to exit and reap it (thread_join kcall).
 *
 * If the target is still running, blocks the caller in STATE_JOIN and records
 * it as the target's joiner; if already dead, returns the exit status and
 * destroys the target. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (tid to join, optional status out)
 * Return value: EOK / ENOERROR per path; ESRCH/EINVAL/EBUSY/EDEADLK/
 *               ETIMEDOUT on failure.
 */
int kdecl ksys_thread_join(thread_t *act, struct kcall_args_thread_join *kap)
{
    thread_t *thp;
    process_t *prp = act->process;
    unsigned tls_flags;
    unsigned claim_spins = 0;
    void *status = NULL;
    int target_dead;
    struct user_slice status_slice;

    /* prp->vec_slock covers lookup, target predicates, and publishing the live
     * joiner. WAIT_PENDING bridges the post-unlock block transition:
     * detach/destroy wakers spin until blocked_on has been restored.
     */

    /* Verify the specified thread exists. */
retry:
    kspinlock_lock(&prp->vec_slock);
    thp = vector_lookup2(&prp->threads, (unsigned) (kap->tid - 1));
    if (!thp) {
        kspinlock_unlock(&prp->vec_slock);
        return ESRCH;
    }
    if (thp->tid != (uint32_t) (kap->tid - 1)) {
        kspinlock_unlock(&prp->vec_slock);
        return ESRCH;
    }
    /* It is invalid to join a detached thread. */
    if (thp->flags & RTMUX_FLG_THR_DETACHED) {
        kspinlock_unlock(&prp->vec_slock);
        return EINVAL;
    }
    /* It is invalid to join a thread which is already being joined. */
    if (thp->join) {
        kspinlock_unlock(&prp->vec_slock);
        return EBUSY;
    }
    /* It is invalid to join to yourself. */
    if (thp == act) {
        kspinlock_unlock(&prp->vec_slock);
        return EDEADLK;
    }
    target_dead = thp->state == STATE_DEAD;

    /* Range-check and probe the status pointer here: after every predicate that
     * can refuse the join, and before thread_reserve_destroy or the thp->join
     * publication below. Both bounds on the placement matter.
     *
     * Later and an EFAULT would orphan published state. The probe this replaces
     * was a no-op WR_PROBE_OPT sitting after thp->join = act, so making it real
     * in place would leave a target pointing at a caller that never blocks.
     *
     * Earlier and every refused join would touch the caller's buffer.
     * xfer_probe's write mode is a per-word read-modify-write, not a passive
     * test, so probing at function entry would store into *kap->status on the
     * ESRCH, EINVAL, EBUSY and EDEADLK paths, and a concurrent writer to that
     * buffer elsewhere in the caller's process would lose its update to the
     * probe's write-back. The predicate this sits next to, WR_PROBE_PTR, is
     * pure and never had that effect.
     *
     * user_slice_init rather than a macro with an embedded return, so the
     * failure path can release vec_slock. Its write check reads and rewrites
     * only the first and last byte, still before any state is published.
     */
    if (user_slice_init(act, kap->status, kap->status ? sizeof status : 0,
                        sizeof status, USER_SLICE_WR, &status_slice)) {
        kspinlock_unlock(&prp->vec_slock);
        return EFAULT;
    }
    if (target_dead && !thread_reserve_destroy(thp)) {
        kspinlock_unlock(&prp->vec_slock);
        if (++claim_spins > 1000000) {
            kprintf("FATAL: thread_join destroy claim stuck tid=%d\n",
                    kap->tid);
            crash();
        }
        thread_join_relax();
        goto retry;
    }
    if (target_dead) {
        status = thp->status;
        atomic_or(&thp->flags, RTMUX_FLG_THR_DETACHED);
        thp->status = 0;
    } else {
        tls_flags = TLS_FLAGS_SAFE(act);

        if (IMTO(act, STATE_JOIN)) {
            kspinlock_unlock(&prp->vec_slock);
            return ETIMEDOUT;
        }

        if (PENDCAN(tls_flags)) {
            kspinlock_unlock(&prp->vec_slock);
            SETKIP_FUNC(act, act->process->canstub);
            return ENOERROR;
        }

        ITF_SET(act, RTMUX_ITF_WAIT_PENDING);
        cpu_smp_mb();
        thp->join = act;
    }
    kspinlock_unlock(&prp->vec_slock);

    /* If the thread is not dead and waiting we block on it. */
    if (!target_dead) {
        unready(act, STATE_JOIN);
        act->blocked_on = thp;
        act->args.jo.statusptr = kap->status;
        cpu_smp_mb();
        ITF_CLR(act, RTMUX_ITF_WAIT_PENDING);
        cpu_smp_mb();
        timeout_deferred_fire(); /* join timeout that expired while arming */
        return ENOERROR;
    }

    /* Return thread exit status to caller. Fault-safe, not a raw store: the
     * probe ran under vec_slock above and the caller can unmap the page between
     * there and here, and this kcall arms no fault handler. The target is
     * reaped either way, because reporting EFAULT while leaving a
     * reserved-for-destroy thread behind would leak it, and the status has
     * already been consumed from the thread under the lock.
     */
    if (user_slice_copyout(&status_slice, &status) != 0) {
        act->timeout_flags = 0;
        thread_destroy(thp);
        return EFAULT;
    }

    act->timeout_flags = 0;
    thread_destroy(thp);

    return EOK;
}


/* Request cancellation of a thread (__thread_cancel kcall).
 *
 * With tid == 0 just records the cancel stub; otherwise sets the target's
 * PTHREAD_CANCEL_PENDING flag and, if cancellation is enabled and the thread is
 * at a cancellation point, forces it ready at the cancel handler. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (tid, canstub)
 * Return value: EOK on success; ESRCH if the tid is absent.
 */
int kdecl ksys_thread_cancel(thread_t *act,
                             struct kcall_args_thread_cancel *kap)
{
    thread_t *thp;
    process_t *prp = act->process;
    unsigned tlsflags = 0;

    /* vec_slock around the vector_lookup; released before the cancelable-check
     * + force_ready / thread_cancel because force_ready takes sched_slock
     * (rank-inversion if held under vec_slock).
     *
     * A foreign target is never touched directly: RTMUX_ATF_CANCEL is posted on
     * its async_flags and its owning hart rewrites its registers. The canstub
     * write is single-pointer atomic with last-writer-wins semantics,
     * acceptable for a process-global config setting.
     */

    /* If tid == 0 then we simply save the cancel stub. */
    if (kap->tid == 0) {
        act->process->canstub = kap->canstub;
        return EOK;
    }

    /* Look the target up and take a lifetime pin under the thread RCU read
     * section (the protocol thread_destroy waits on), then drop the section:
     * force_ready takes sched_slock, which must not be nested under it. The
     * target may be running on another hart, so this function never writes its
     * registers; it posts RTMUX_ATF_CANCEL and the owning hart does the
     * redirect.
     */
    if (kap->tid == (int32_t) (act->tid + 1)) {
        thp = act;
    } else {
        rcu_read_lock(&tthread_rcu);
        thp = vector_lookup(&prp->threads, (unsigned) (kap->tid - 1));
        if (thp && thp->tid != (uint32_t) (kap->tid - 1))
            thp = NULL;
        if (thp && tthread_pin(thp) != EOK)
            thp = NULL;
        rcu_read_unlock(&tthread_rcu);
        if (!thp)
            return ESRCH;
    }

    /* Check for WAAA as stack (where tls is usually located) may not have a
     * stack yet.
     */
    if (!(thp->flags & RTMUX_FLG_THR_WAAA)) {
        /* The target's TLS is user memory it can unmap under us: publish
         * PENDING and read the old flags in one fault-safe atomic access (so
         * the cancel decision cannot see flags older than the publish), and
         * refuse the cancel before anything else is published if the page is
         * gone.
         */
        if (xfer_fetch_or_user((volatile unsigned *) &thp->un.lcl.tls->__flags,
                               PTHREAD_CANCEL_PENDING, &tlsflags) != 0) {
            if (thp != act)
                tthread_unpin(thp);
            return EFAULT;
        }
    }

    thp->process->canstub = kap->canstub;

    /* If cancelation is enabled and we are blocked, force thread ready at
     * address of cancelation handler. We also make thread birth a cancellation
     * point.
     */
    if ((thp->flags & RTMUX_FLG_THR_WAAA) ||
        ((tlsflags & PTHREAD_CANCEL_DISABLE) == 0 &&
         (STATE_CANCELABLE(thp) || (tlsflags & PTHREAD_CANCEL_ASYNCHRONOUS)))) {
        if (thp == act) {
            force_ready(thp, EINTR);
            thread_cancel(thp);
        } else {
            atomic_fetch_or_explicit(&thp->async_flags, RTMUX_ATF_CANCEL,
                                     memory_order_release);
            force_ready(thp, EINTR);
            if (thp->state == STATE_RUNNING)
                SENDIPI(thp->runcpu, IPI_RESCHED);
        }
    }

    if (thp != act)
        tthread_unpin(thp);
    return EOK;
}


/* Thread control operations on the calling thread (thread_ctl kcall).
 *
 * Thin wrapper that applies the operation to the caller itself. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (cmd + command-specific data)
 * Return value: the kcall_op_thread_ctl result.
 */
int kdecl ksys_thread_ctl(thread_t *act, struct kcall_args_thread_ctl *kap)
{
    return kcall_op_thread_ctl(act, act, kap);
}

/* Break an interruptible blocked call of a thread in the caller's process with
 * EINTR (RTMUX_TCTL_INTERRUPT).
 *
 * The target is found by tid in the caller's own process only, so no authority
 * is conferred: any thread may already cancel or hold its siblings. Lookup and
 * lifetime pin follow thread_cancel (thread RCU section, then tthread_pin, then
 * the section is dropped because force_ready takes sched_slock). The state test
 * is lock-free, as in thread_cancel; a target that left its wait in between is
 * a no-op in force_ready, and the caller sees EOK for a wait that ended anyway.
 * The wakeup is the ordinary one: force_ready unlinks the waiter under its
 * channel/sync lock and ready() queues it on its own hart's dispatch with the
 * usual reschedule IPI, so a target blocked on another hart needs no extra
 * cross-hart step and no register rewrite (unlike cancel).
 *
 * Parameters:
 *   act : calling thread (selects the process)
 *   tid : 1-based tid of the target
 * Return value: EOK when the wait was broken (or a server unblock was queued),
 *               EAGAIN when the target is not in an interruptible wait, ESRCH
 *               for an unknown tid, EINVAL for a bad tid.
 */
static int thread_interrupt(thread_t *act, int tid)
{
    process_t *prp = act->process;
    thread_t *thp;
    int r = EAGAIN;

    if (tid <= 0)
        return EINVAL;
    if (tid == (int) (act->tid + 1))
        return EAGAIN; /* the caller is running, not blocked */

    rcu_read_lock(&tthread_rcu);
    thp = vector_lookup(&prp->threads, (unsigned) (tid - 1));
    if (thp && thp->tid != (uint32_t) (tid - 1))
        thp = NULL;
    if (thp && tthread_pin(thp) != EOK)
        thp = NULL;
    rcu_read_unlock(&tthread_rcu);
    if (!thp)
        return ESRCH;

    if (STATE_INTERRUPTIBLE(thp)) {
        force_ready(thp, EINTR);
        r = EOK;
    }
    tthread_unpin(thp);
    return r;
}

/* Runmask width, in 32-bit words.
 *
 * Assumption class: architectural, tied to the RTMUX_TCTL_RUNMASK_* ABI. A
 * runmask is a single 32-bit word, so the scheduler addresses at most 32 harts,
 * and `struct _thread_runmask.size` is 1 for every caller in the tree. The `#if
 * RMSK_MAX > 1` arms below sketch multi-word support and have never been
 * compiled; treat them as a design note, not as working code.
 *
 * Revisit trigger: a target with more than 32 harts. Raising
 * CONFIG_PROCESSORS_MAX past 32 without widening the runmask would silently
 * mis-mask, because LEGAL_CPU_BITMASK is a 32-bit quantity and the lastmask
 * shift below would wrap (RISC-V masks a shift count to 0x1f, so a shift of 32
 * is a shift of 0). The assertion makes that a build failure instead.
 */
#define RMSK_MAX 1

_Static_assert(RMSK_MAX > 1 || CONFIG_PROCESSORS_MAX <= 32,
               "CONFIG_PROCESSORS_MAX > 32 needs a multi-word runmask: "
               "LEGAL_CPU_BITMASK and RTMUX_TCTL_RUNMASK_* are 32 bits wide");

/* Apply a thread control operation to a target thread.
 *
 * Implements the RTMUX_TCTL_* commands: IO privilege, thread hold/cont, runmask
 * get/set (with inherit), alignment-fault toggle, and thread naming. The vendor
 * extension hook gets first refusal. Parameters:
 *   act : the calling (active) thread
 *   op  : the thread the operation acts on (may equal act)
 *   kap : kcall args (cmd + command-specific data)
 * Return value: EOK / ENOERROR on success; an errno on failure.
 */
int kcall_op_thread_ctl(thread_t *act,
                        thread_t *op,
                        struct kcall_args_thread_ctl *kap)
{
    int prev;
    unsigned new[RMSK_MAX], inherit[RMSK_MAX], *uintp;
    int r, set_inherit, set_rmask;
    unsigned num;

    /* Kernel copies of the user structures; the user buffers are reached only
     * through user_slice copies, never dereferenced.
     */
    struct _thread_name tn_copy, *tn = &tn_copy;
    struct {
        struct _thread_runmask hdr;
        unsigned words[2 * RMSK_MAX];
    } rm_copy;
    struct _thread_runmask *tr = &rm_copy.hdr;
    char name_in[RTMUX_FLG_THREAD_NAME_MAX];
    char name_out[RTMUX_FLG_THREAD_NAME_MAX + 1];
    size_t name_out_len;
    struct user_slice in_slice, out_slice;
    char *prev_name;
    size_t prev_name_len;
    struct _thread_destroy_stats tds;
#if RMSK_MAX > 1
    int lastmask;
#endif


    /* Give the vendor extension a chance to take first dibs */
    if (kcall_op_thread_ctl_hook) {
        r = kcall_op_thread_ctl_hook(act, op, kap);
        if (r != ENOSYS) {
            return r;
        }
    }

    switch (kap->cmd) {
    case RTMUX_TCTL_IO:
        if (act != op) {
            return ENOTSUP;
        }

        /* The IO privilege is a hardware-interrupt authority, granted by
         * RTMUX_PRIV_CAP_IRQ rather than the root credential, so it derives
         * from the bootstrap manifest. A process without the cap is refused
         * with EPERM, matching the former root check (the cred_is_root helper
         * raised EPERM on a non-root caller). A child holds every cap while
         * RTMUX_FLG_PROC_LOADING; exclude that window so a loading child cannot
         * take IO privilege, matching the former root-only semantics.
         */
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_IRQ)) {
            return EPERM;
        }
        /* RTMux has no demand paging, all memory is already locked. */
        cpu_thread_priv(act);
        atomic_or(&act->flags, RTMUX_FLG_THR_IOPRIV);
        break;

    case RTMUX_TCTL_NATIVE_REPLY:
        if (act != op)
            return ENOTSUP;
        if (kap->data)
            ITF_SET(act, RTMUX_ITF_NATIVE_REPLY);
        else
            ITF_CLR(act, RTMUX_ITF_NATIVE_REPLY);
        break;

    case RTMUX_TCTL_INTERRUPT:
        return thread_interrupt(act, (int) (intptr_t) kap->data);

    case RTMUX_TCTL_ONE_THREAD_HOLD:
        if (!stop_one_thread(op->process, (int) (intptr_t) kap->data - 1,
                             RTMUX_FLG_THR_THREADS_HOLD, 0)) {
            return ESRCH;
        }
        atomic_clr(&op->flags,
                   (RTMUX_FLG_THR_TO_BE_STOPPED | RTMUX_FLG_THR_THREADS_HOLD));
        break;

    case RTMUX_TCTL_THREADS_HOLD:
        stop_threads(op->process, RTMUX_FLG_THR_THREADS_HOLD, 0);
        atomic_clr(&op->flags,
                   (RTMUX_FLG_THR_TO_BE_STOPPED | RTMUX_FLG_THR_THREADS_HOLD));
        break;

    case RTMUX_TCTL_ONE_THREAD_CONT:
        if (!cont_one_thread(op->process, (int) (intptr_t) kap->data - 1,
                             RTMUX_FLG_THR_THREADS_HOLD)) {
            return ESRCH;
        }
        break;

    case RTMUX_TCTL_THREADS_CONT:
        cont_threads(op->process, RTMUX_FLG_THR_THREADS_HOLD);
        break;

    case RTMUX_TCTL_RUNMASK:
        if (set_runmask(op, (unsigned) (uintptr_t) kap->data) != 0) {
            return ENOERROR;
        }
        break;
    case RTMUX_TCTL_RUNMASK_GET_AND_SET_INHERIT:

        num = RMSK_SIZE(num_processors);
#ifndef NDEBUG
        if (num > RMSK_MAX)
            crash();
#endif

        /* Copy the whole extent in before any member is read: NULL, an
         * out-of-range address and an in-range unmapped page are all refused by
         * the slice, and this kcall arms no xfer fault handler of its own. The
         * kernel then works on rm_copy, never on the user buffer.
         */
        if (user_slice_init(
                act, kap->data,
                sizeof(struct _thread_runmask) + 2 * num * sizeof(unsigned),
                sizeof rm_copy, USER_SLICE_RD, &in_slice) ||
            user_slice_copyin(&rm_copy, &in_slice))
            return EFAULT;

        /* We force them to initialize tr->num with RMSK_SIZE(). This is so
         * there's no ambiguity as to how many bits are valid and / or where the
         * second array starts on return. This could occur if they were to pass
         * in a large tr->size.
         */
        if (tr->size != num) {
            return EINVAL;
        }

        set_rmask = 0;
        set_inherit = 0;
        uintp = rm_copy.words;
#if RMSK_MAX > 1

        /* This will have to be propagated to / synced with other areas.
         * - Currently more error checking and validation is done up
         *   front here rather than other areas:
         * - This assumes we take the mask as is rather than an inverted
         *   copy.
         * - set_runmask()
         */
        /* Multi-word runmask sketch; see the RMSK_MAX banner. Never compiled.
         */
        memcpy(new, uintp, num * sizeof(unsigned));
        memset(new + num, 0x00, (RMSK_MAX - num) * sizeof(unsigned));
        uintp += num;
        memcpy(inherit, uintp, num * sizeof(unsigned));
        memset(inherit + num, 0x00, (RMSK_MAX - num) * sizeof(unsigned));

        for (r = 0; r < num - 1; r++) {
            if (new[r] != 0)
                set_rmask = 1;
            if (inherit[r] != 0)
                set_inherit = 1;
        }

        /* Wraps when num_processors == 32 && r == 0: RISC-V masks a shift count
         * to 0x1f, so a shift of 32 is a shift of 0. Guarded by the RMSK_MAX
         * static assertion rather than fixed here, since this arm is
         * uncompiled.
         */
        lastmask = (1 << (num_processors - (r * __INT_BITS__))) - 1;

        if (set_rmask == 0 && new[r] != 0) {
            if ((new[r] & lastmask) == 0)
                return EINVAL;
            set_rmask = 1;
        }
        if (set_inherit == 0 && inherit[r] != 0) {
            if ((inherit[r] & lastmask) == 0)
                return EINVAL;
            set_inherit = 1;
        }
        new[r] &= lastmask;
        inherit[r] &= lastmask;
#else
        new[0] = *uintp;
        uintp++;
        inherit[0] = *uintp;

        if (inherit[0] != 0) {
            if ((inherit[0] & LEGAL_CPU_BITMASK) == 0) {
                return EINVAL;
            }
            set_inherit = 1;
        }

        /* set_runmask() currently does the above check on the mask proper. */
        if (new[0] != 0)
            set_rmask = 1;
#endif

        /* Check the write extent now; the copy-out below runs before
         * set_runmask() so a refused buffer leaves the masks untouched.
         */
        if (user_slice_init(
                act, kap->data,
                sizeof(struct _thread_runmask) + 2 * num * sizeof(unsigned),
                sizeof rm_copy, USER_SLICE_WR, &out_slice))
            return EFAULT;

        tr->size = num;
        uintp = rm_copy.words;
#if RMSK_MAX > 1

        /* Multi-word get is unimplemented; set_runmask() takes one word. See
         * the RMSK_MAX banner.
         */
#else
        *uintp = (unsigned) (~op->runmask & LEGAL_CPU_BITMASK);
        uintp++;
        *uintp = (unsigned) (~op->default_runmask & LEGAL_CPU_BITMASK);
        if (user_slice_copyout(&out_slice, &rm_copy))
            return EFAULT;
        if (set_rmask != 0 && set_runmask(op, new[0]) != 0) {
            return ENOERROR;
        }
        if (set_inherit)
            op->default_runmask = ~inherit[0];
#endif
        break;


    case RTMUX_TCTL_RUNMASK_GET_AND_SET:
        if (user_slice_init(act, kap->data, sizeof(unsigned), sizeof(unsigned),
                            USER_SLICE_WR, &in_slice) ||
            user_slice_copyin(&new[0], &in_slice))
            return EFAULT;
        {
            unsigned old = (unsigned) (~op->runmask & LEGAL_CPU_BITMASK);

            if (user_slice_copyout(&in_slice, &old))
                return EFAULT;
        }
        if (new[0] != 0 && set_runmask(op, new[0]) != 0) {
            return ENOERROR;
        }
        break;
    case RTMUX_TCTL_ALIGN_FAULT:
        if (user_slice_init(act, kap->data, sizeof(int), sizeof(int),
                            USER_SLICE_WR, &in_slice) ||
            user_slice_copyin(&r, &in_slice))
            return EFAULT;
        prev = (op->flags & RTMUX_FLG_THR_ALIGN_FAULT) ? 1 : -1;
        if (r > 0) {
            atomic_or(&op->flags, RTMUX_FLG_THR_ALIGN_FAULT);
        } else if (r < 0) {
            atomic_clr(&op->flags, RTMUX_FLG_THR_ALIGN_FAULT);
        }
        cpu_thread_align_fault(op);

        /* The flag is already applied; a page unmapped by a sibling hart since
         * the check above reports EFAULT with the new setting in effect.
         */
        if (user_slice_copyout(&in_slice, &prev))
            return EFAULT;
        break;

    case RTMUX_TCTL_NAME:

        if (user_slice_get(act, tn, kap->data, sizeof *tn))
            return EFAULT;
        if (tn->new_name_len > RTMUX_FLG_THREAD_NAME_MAX) {
            return E2BIG;
        }

        if (tn->name_buf_len < 0 || tn->new_name_len > tn->name_buf_len) {
            return EINVAL;
        }

        /* name_buf is the inline tail of the user structure. The new name is
         * bounded by RTMUX_FLG_THREAD_NAME_MAX above; the output extent keeps
         * the caller's own length (any int), as the old range check did.
         */
        {
            const char *nb = (const char *) kap->data +
                             offsetof(struct _thread_name, name_buf);

            if (user_slice_init(
                    act, nb,
                    tn->new_name_len > 0 ? (size_t) tn->new_name_len : 0,
                    sizeof name_in, USER_SLICE_RD, &in_slice) ||
                user_slice_copyin(name_in, &in_slice))
                return EFAULT;
            if (user_slice_init(
                    act, nb,
                    tn->name_buf_len > 0 ? (size_t) tn->name_buf_len : 0,
                    (size_t) INT_MAX, USER_SLICE_WR, &out_slice))
                return EFAULT;
        }

        prev_name = op->name;
        prev_name_len = (op->name) ? strlen(op->name) + 1 : 0;

        /* Stage and copy out the previous (or current) name before the name is
         * replaced, so a refused buffer leaves the thread name unchanged.
         */
        name_out_len = 0;
        if (tn->name_buf_len > 0) {
            size_t avail = (size_t) tn->name_buf_len;

            if (avail > sizeof name_out)
                avail = sizeof name_out;
            if (prev_name_len > avail) {
                memcpy(name_out, prev_name, avail - 1);
                name_out[avail - 1] = '\0';
                name_out_len = avail;
            } else if (prev_name_len) {
                memcpy(name_out, prev_name, prev_name_len);
                name_out_len = prev_name_len;
            } else {
                name_out[0] = '\0'; /* No name, return null string */
                name_out_len = 1;
            }
            /* A prefix of the validated extent is itself validated. */
            out_slice.len = name_out_len;
            if (user_slice_copyout(&out_slice, name_out))
                return EFAULT;
        }

        /* Setting a new name for the thread NOTE: Due to the nature of the
         * incoming/outgoing names we always re-allocate a buffer here since
         * otherwise we would have to allocate a temporary buffer in order to be
         * able to return the previous name to the user. TODO: Investigate a
         * character by character copy to avoid allocations.
         */
        if (tn->new_name_len >= 0) {
            if (tn->new_name_len >= THREAD_NAME_FIXED_SIZE) {
                op->name = _smalloc((unsigned) tn->new_name_len + 1);
            } else if (tn->new_name_len > 0) {
                op->name = object_alloc(op->process, &threadname_souls);
            } else {
                op->name = NULL;
            }

            if (op->name) {
                memcpy(op->name, name_in, (unsigned) tn->new_name_len);
                op->name[tn->new_name_len] = '\0'; /* Force a null */
            }

            _TRACE_TH_EMIT_NAME(op);
        }
        /* Deallocate previous name if we changed it */
        if (prev_name_len && prev_name != op->name) {
            if (prev_name_len >= THREAD_NAME_FIXED_SIZE) {
                _sfree(prev_name, prev_name_len);
            } else {
                object_free(op->process, &threadname_souls, prev_name);
            }
            prev_name = NULL;
        }
        break;

    case RTMUX_TCTL_THREAD_DESTROY_STATS:
        if (act->process->cred->info.euid != 0)
            return EPERM;
        if (user_slice_init(act, kap->data, sizeof tds, sizeof tds,
                            USER_SLICE_WR, &in_slice) ||
            user_slice_copyin(&tds, &in_slice))
            return EFAULT;
        thread_destroy_stats_snapshot(&tds, (int) tds.reset);
        if (user_slice_copyout(&in_slice, &tds))
            return EFAULT;
        break;

    case RTMUX_TCTL_PERFCOUNT:
    default:
        return EINVAL;
    }

    return EOK;
}
