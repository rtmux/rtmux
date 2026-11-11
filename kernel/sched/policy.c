/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <user-slice.h>

/* Convert a nanosecond count into a struct timespec.
 *
 * Parameters:
 *   ts   : destination timespec
 *   nsec : nanoseconds to convert
 */
void nsec2timespec(struct timespec *ts, uint64_t nsec)
{
    ts->tv_sec = (time_t) (nsec / (uint64_t) 1000000000);
    ts->tv_nsec = (long) (nsec % (uint64_t) 1000000000);
}

static int sched_set_noop_status(thread_t *thp,
                                 int policy,
                                 const struct sched_param *param,
                                 int *status)
{
    if (policy == RTMUX_SCHED_NOCHANGE) {
        policy = thp->policy;
    } else if (policy == RTMUX_SCHED_SETPRIO) {
        if (param->sched_priority <= 0 || param->sched_priority >= NUM_PRI) {
            *status = EINVAL;
            return 1;
        }
        if (!(thp->process->priv_caps & RTMUX_PRIV_CAP_SCHED) &&
            param->sched_priority >= kopt.priv_prio) {
            *status = EPERM;
            return 1;
        }
        if (thp->policy == SCHED_SPORADIC)
            return 0;
        *status = EOK;
        return param->sched_priority == thp->real_priority &&
               thp->priority == thp->real_priority;
    }
    if (policy == SCHED_SPORADIC || policy == RTMUX_SCHED_ADJTOHEAD ||
        policy == RTMUX_SCHED_ADJTOTAIL)
        return 0;
    *status = ksched_ok(thp, policy, param);
    if (*status != EOK)
        return 1;
    *status = EOK;
    return policy == thp->policy &&
           param->sched_priority == thp->real_priority &&
           thp->priority == thp->real_priority;
}

/* Sched territory. Live kcalls: 88 (sched_get), 89 (sched_set), 90
 * (sched_yield), 91 (sched_info).
 *
 * Shared state:
 *   - prp->threads vector_lookup (residual: vec_slock not held —
 *     thread/connect lookup pattern).
 *   - thp->{policy, priority, real_priority, schedinfo.*} — read-only
 *     in sched_get; written via adjust_priority / sched_thread under
 *     sched_slock.
 *   - prp->process_priority — written in sched_set (no slock;
 *     residual).
 *   - act->state via yield() — sched_slock-internal.
 *
 * process_t RCU coverage: scheduler kcalls are process-local.
 */

/* Get a thread's scheduling policy and parameters (sched_get kcall).
 *
 * Fills kap->param with the target thread's current/base priority and, for
 * SCHED_SPORADIC, its sporadic parameters. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (target pid/tid, param buffer)
 * Return value: ENOERROR on success (policy in KSTATUS); ESRCH if absent.
 */
int kdecl ksys_sched_get(thread_t *act, struct kcall_args_sched_get *kap)
{
    process_t *prp;
    thread_t *thp;
    struct sched_param param;
    int policy;
    int thp_rcu = 0;
    struct user_slice param_slice;

    if (kap->param &&
        user_slice_init(act, kap->param, sizeof(param), sizeof(param),
                        USER_SLICE_WR, &param_slice))
        return EFAULT;

    if (kap->pid == 0 || kap->pid == act->process->pid) {
        prp = act->process;
    } else {
        prp = lookup_pid_rcu(kap->pid);
        rcu_sched_read_unlock();
    }
    if (prp != act->process)
        return EPERM;

    /* Verify the target thread exists. */
    if (kap->tid == (int32_t) (act->tid + 1) && prp == act->process) {
        thp = act;
    } else if (kap->tid) {
        rcu_read_lock(&tthread_rcu);
        thp_rcu = 1;
        thp = vector_lookup(&prp->threads, (unsigned) (kap->tid - 1));
        if (thp && thp->tid != (uint32_t) (kap->tid - 1))
            thp = NULL;
    } else {
        thp = act;
    }
    if (!thp) {
        if (thp_rcu)
            rcu_read_unlock(&tthread_rcu);
        return ESRCH;
    }

    memset(&param, 0, sizeof(param));
    if (kap->param) {
        param.sched_curpriority = thp->priority;
        param.sched_priority = thp->real_priority;

        /* thp may be a same-process sibling running on another hart (the caller
         * passed its tid), so a concurrent sched_ss_cleanup() there could free
         * ss_info and flip policy mid-read. Bracket with the ticker_using
         * reader count that WAIT_FOR_CLOCK_DONE() waits on, and snapshot policy
         * inside the bracket so the returned policy stays consistent with the
         * params filled here — reading policy outside could report SPORADIC
         * while the params come back zeroed. Load ss_info once and NULL-check
         * it. The atomic add/sub tolerates a timer tick nesting when thp ==
         * act.
         */
        __atomic_fetch_add(&thp->ticker_using, 1u, __ATOMIC_RELAXED);
        __cpu_membarrier();
        policy = thp->policy;
        if (policy == SCHED_SPORADIC) {
            sched_info_t *ss = thp->schedinfo.ss_info;
            if (ss) {
                if (ss->org_priority) {
                    /* Running at low priority - return the original
                     * sched_priority
                     */
                    param.sched_priority = ss->org_priority;
                }
                param.ss_low_priority = ss->low_priority;
                param.ss_max_repl = ss->max_repl;
                nsec2timespec(&param.ss_init_budget, ss->init_budget);
                nsec2timespec(&param.ss_repl_period, ss->repl_period);
            }
        }
        __cpu_membarrier();
        __atomic_fetch_sub(&thp->ticker_using, 1u, __ATOMIC_RELAXED);
    } else {
        policy = thp->policy;
    }

    if (thp_rcu)
        rcu_read_unlock(&tthread_rcu);
    if (kap->param && user_slice_copyout(&param_slice, &param))
        return EFAULT;
    SETKSTATUS(act, policy);

    return ENOERROR;
}


/* Set a thread's (or process's) scheduling policy and parameters (__sched_set
 * kcall).
 *
 * With a specific tid, applies the policy/param change to that thread via
 * sched_thread; with tid == -1, makes a process-wide priority adjustment.
 * Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (target pid/tid, policy, param)
 * Return value: EOK on success; an errno on failure.
 */
int kdecl ksys_sched_set(thread_t *act, struct kcall_args_sched_set *kap)
{
    process_t *prp;
    thread_t *thp;
    struct sched_param param;
    int status;
    int prp_pinned = 0;
    int thp_pinned = 0;

    if (kap->pid == 0 || kap->pid == act->process->pid) {
        prp = act->process;
    } else {
        prp = lookup_pid_rcu(kap->pid);
        rcu_sched_read_unlock();
    }
    if (prp != act->process)
        return EPERM;

    /* Verify the target thread exists, or find one for verification. */
    if (kap->tid != -1) {
        if (kap->tid == (int32_t) (act->tid + 1) && prp == act->process) {
            thp = act;
        } else if (kap->tid) {
            rcu_read_lock(&tthread_rcu);
            thp = vector_lookup(&prp->threads, (unsigned) (kap->tid - 1));
            if (thp && thp->tid != (uint32_t) (kap->tid - 1))
                thp = NULL;
            rcu_read_unlock(&tthread_rcu);
        } else {
            thp = act;
        }
        if (!thp) {
            return ESRCH;
        }
    } else {
        if (kap->policy != RTMUX_SCHED_NOCHANGE)
            return EINVAL;
        thp = NULL;
    }

    switch (kap->policy) {
    case RTMUX_SCHED_ADJTOHEAD:
    case RTMUX_SCHED_ADJTOTAIL:
        memset(&param, 0, sizeof(param));
        break;
    case SCHED_SPORADIC:
        if (!kap->param) {
            return EINVAL;
        }
        if (user_slice_get(act, &param, kap->param, sizeof(param)))
            return EFAULT;
        break;
    default:
        if (!kap->param) {
            return EINVAL;
        }
        memset(&param, 0, sizeof(param));
        if (user_slice_get(act, &param.sched_priority,
                           &kap->param->sched_priority,
                           sizeof(param.sched_priority)))
            return EFAULT;
        break;
    }

    if (process_pin(prp) != EOK)
        return ESRCH;
    prp_pinned = 1;

    if (kap->tid > 0) {
        if (kap->tid == (int32_t) (act->tid + 1) && prp == act->process) {
            thp = act;
        } else {
            rcu_read_lock(&tthread_rcu);
            thp = vector_lookup(&prp->threads, (unsigned) (kap->tid - 1));
            if (thp && thp->tid != (uint32_t) (kap->tid - 1))
                thp = NULL;
            if (thp && tthread_pin(thp) != EOK)
                thp = NULL;
            if (thp)
                thp_pinned = 1;
            rcu_read_unlock(&tthread_rcu);
        }
        if (!thp) {
            status = ESRCH;
            goto out;
        }
    }

    /* Either make a process wide priority adjustment or apply the parameter
     * change to only one thread as targetted.
     */
    if (!thp) {
        int delta;

        /* NYI: no range checking being done.... */
        delta = param.sched_priority - prp->process_priority;
        prp->process_priority = (uint8_t) (prp->process_priority + delta);

        /* @@@ Run the entire thread list adjusting by the delta amount?
         * PROBLEM: sched_thread does more range checking than adjust_priority
         * does. for(tid = 0 ; tid < prp->threads.nentries ; ++tid) { if(thp =
         * VECP(thp, &prp->threads, tid)) { adjust_priority(thp, thp->priority +
         * delta, thp->dpp); thp->real_priority += delta; } }
         */
    } else if (sched_set_noop_status(thp, kap->policy, &param, &status)) {
        if (status != EOK) {
            goto out;
        }
    } else if ((status = sched_thread(thp, kap->policy, &param)) != EOK) {
        goto out;
    }

    status = EOK;
out:
    if (thp_pinned)
        tthread_unpin(thp);
    if (prp_pinned)
        process_unpin(prp);
    return status;
}

/* Report the priority range and interval for a scheduling policy (sched_info
 * kcall).
 *
 * Fills kap->info with min/max/privileged priorities (per the caller's
 * privilege) and, for round-robin policies, the timeslice interval. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (optional pid, policy, info buffer)
 * Return value: EOK on success; an errno on failure.
 */
int kdecl ksys_sched_info(thread_t *act, struct kcall_args_sched_info *kap)
{
    process_t *prp;
    struct _sched_info si, *sip = &si;
    struct user_slice info_slice;

    if (kap->pid == 0 || kap->pid == act->process->pid) {
        prp = act->process;
    } else {
        prp = lookup_pid_rcu(kap->pid);
        rcu_sched_read_unlock();
    }
    if (prp != act->process)
        return EPERM;

    if (user_slice_init(act, kap->info, sizeof(si), sizeof(si), USER_SLICE_WR,
                        &info_slice))
        return EFAULT;

    /* Set min to 1 and max to number of priorities */
    memset(sip, 0x00, sizeof *sip);
    if (act->process->priv_caps & RTMUX_PRIV_CAP_SCHED) {
        sip->priority_max = NUM_PRI - 1;
    } else {
        sip->priority_max = (int) (kopt.priv_prio - 1);
    }
    sip->priority_min = 1; /* Only idle allowed at prio 0 */
    sip->priority_priv = (int) (kopt.priv_prio - 1);

    /* Verify the policy */
    switch (kap->policy) {
    case SCHED_RR:
    case SCHED_OTHER:
        sip->interval = qtimeptr->nsec_inc * RTMUX_RR_INTERVAL_MUL;
        /* Fall Through */
    case SCHED_SPORADIC:
    case SCHED_FIFO:
        break;
    default:
        return EINVAL;
    }

    return user_slice_copyout(&info_slice, sip) ? EFAULT : EOK;
}


int kdecl ksys_sched_yield(thread_t *act, struct kcall_args_null *kap)
{
    /* #90 __sched_yield. yield() is sched_slock-internal; preempt fence covers
     * the function body.
     */
    (void) act;
    (void) kap;
    yield();
    return EOK;
}
