/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <user-slice.h>

/* Don't use top bit so the clockid_t is positive */
#define CLOCK_ID_RUNTIME 0x40000000

#define MAKE_CLOCKID(pid, tid) \
    (CLOCK_ID_RUNTIME | (PINDEX(pid) << 16) | ((tid) & 0xffff))

/* This kernel call allows you to get and/or set the time. It also can return
 * the running time of this process or one of its threads. The time is provided
 * in seconds and nanoseconds.
 */
int kdecl ksys_clock_time(thread_t *act, struct kcall_args_clock_time *kap)
{
    uint64_t old, new;
    clockid_t id;

    id = kap->id;

    switch (id) {
    case CLOCK_PROCESS_CPUTIME_ID:
        id = MAKE_CLOCKID(act->process->pid, 0);
        break;
    case CLOCK_THREAD_CPUTIME_ID:
        id = MAKE_CLOCKID(act->process->pid, act->tid + 1);
        break;
    default:
        break;
    }

    if (id & CLOCK_ID_RUNTIME) {
        unsigned pidx;
        unsigned tid;
        process_t *prp;
        thread_t *thp;
        volatile uint64_t *rtp;

        /* Get the running time of a process or thread */

        if (kap->new) {
            return EINVAL;
        }
        pidx = SYNC_PINDEX(id);
        tid = SYNC_TID(id);
        if (pidx != PINDEX(act->process->pid))
            return EPERM;
        thp = NULL;

        /* RCU section spans the process_vector lookup, the vector_lookup on
         * prp->threads (thp's lifetime is also tied to prp's), and the
         * running_time read. The check/probe/copy-out sits after the unlock
         * below: "old" is a local snapshot by then, so nothing in the copy-out
         * needs the section, and the probe's EFAULT return does not have to
         * unwind one.
         */
        rcu_read_lock(&process_rcu);
        if ((pidx == 0) || (pidx >= process_vector.nentries) ||
            !VECP(prp, &process_vector, pidx)) {
            rcu_read_unlock(&process_rcu);
            return ESRCH;
        }
        if ((tid + 1) == 0) {
            /* Get process running time. The banked value lacks the caller's
             * own unbilled stretch (it is running, on this hart), so add it;
             * threads of the process running on other harts contribute only
             * what they have banked.
             */
            rtp = &prp->running_time;
            thp = NULL;
        } else {
            thp = vector_lookup(&prp->threads, tid);
            if (!thp) {
                rcu_read_unlock(&process_rcu);
                return ESRCH;
            }
            rtp = &thp->running_time;
        }

        /* Loop until we get a consistent time */
        do {
            old = *rtp;
        } while (old != *rtp);

        /* Add the live, not-yet-billed stretch of a running thread, read under
         * the owner hart's pc_account_seq seqcount (sched_account_read falls
         * back to the banked value after three torn attempts). Read-only, so
         * the owner-hart-writes invariant of sched_account_current() holds.
         */
        {
            uint64_t now;

            snap_time(&now, 0);
            if (thp)
                old = sched_account_read(thp, now);
            else /* act is on this hart: no concurrent writer of its time */
                old += sched_account_read(act, now) - act->running_time;
        }

        rcu_read_unlock(&process_rcu);

        if (kap->old && user_slice_put(act, kap->old, &old, sizeof(*kap->old)))
            return EFAULT;
        return EOK;
    }

    if ((id != CLOCK_REALTIME) && ((id != CLOCK_MONOTONIC) || kap->new)) {
        return EINVAL;
    }
    /* Make local copy incase kap->new==kap->old since we change old. */
    if (kap->new) {
        if (user_slice_get(act, &new, kap->new, sizeof(new)))
            return EFAULT;
    } else {
        new = 0;
    }

    /* The clock_slock (INTR-spinlock) protects qtp->{adjust, nsec_tod_adjust}
     * writes; the preempt fence covers the snap_time
     * + SETKSTATUS sequence and the kap->new copy-out window (kap->new and
     * kap->old may alias). Get current time.
     */
    if (kap->old) {
        uint64_t now;

        snap_time(&now, id != CLOCK_MONOTONIC);
        if (user_slice_put(act, kap->old, &now, sizeof(now)))
            return EFAULT;
    }

    SETKSTATUS(act, 0);

    /* Set new time. Setting the clock is a privileged time authority, gated by
     * RTMUX_PRIV_CAP_TIME rather than the root credential; a setter without the
     * cap, or a still-loading one, is refused with EPERM, while a reader is
     * unaffected.
     */
    if (kap->new) {
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_TIME))
            return EPERM;

        systime_t *qtp = qtimeptr;

        INTR_LOCK(&clock_slock);

        /* Kill any adjustment in progress */
        memset(&qtp->adjust, 0, sizeof(qtp->adjust));

        qtp->nsec_tod_adjust = new - qtp->nsec;

        INTR_UNLOCK(&clock_slock);

        /* The TOD step changed every TOD-based timer's monotonic-equivalent
         * deadline, so the cached earliest is stale. Drop it (rather than walk
         * the wheel here); the periodic tick rebuilds it within nsec_inc.
         */
        timer_deadline_invalidate();
    }
    return ENOERROR;
}


/* This kernel call allows you to get and/or set the tick size of the clock. */
int kdecl ksys_clock_period(thread_t *act, struct kcall_args_clock_period *kap)
{
    struct _clockperiod new;

    new.nsec = 0;
    new.fract = 0;

    if (kap->id != CLOCK_REALTIME) {
        return EINVAL;
    }
    /* Make local copy incase kap->new==kap->old since we change old. */
    if (kap->new) {
        if (user_slice_get(act, &new, kap->new, sizeof(new)))
            return EFAULT;
    }

    /* clock_resolution + timer_period mutate global tick state and require
     * RTMUX_PRIV_CAP_TIME to set. Get current resolution.
     */
    if (kap->old) {
        struct _clockperiod cur = {.nsec = qtimeptr->nsec_inc, .fract = 0};

        if (user_slice_put(act, kap->old, &cur, sizeof(cur)))
            return EFAULT;
    }

    KSTATUS(act) = 0;

    /* Set new resolution. Setting the clock period is a privileged time
     * authority, gated by RTMUX_PRIV_CAP_TIME rather than the root credential;
     * a setter without the cap, or a still-loading one, is refused with EPERM.
     */
    if (kap->new) {
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_TIME))
            return EPERM;
        if ((new.nsec < tick_minimum) || (new.nsec > RTMUX_TICKSIZE_MAX)) {
            return EINVAL;
        }

        clock_resolution(new.nsec);
        timer_period();
    }
    return ENOERROR;
}



/* This kernel call returns a clock id that can be used to obtain the running
 * time of a process or thread.
 */
int kdecl ksys_clock_id(thread_t *act, struct kcall_args_clock_id *kap)
{
    unsigned id;
    int tid;
    process_t *prp;
    thread_t *thp;
    int prp_rcu = 0;
    int thp_rcu = 0;

    tid = kap->tid;
    if ((kap->pid == 0 || kap->pid == act->process->pid) &&
        (tid == 0 || tid == (int) (act->tid + 1))) {
        KSTATUS(act) = MAKE_CLOCKID(act->process->pid, tid);
        return ENOERROR;
    }

    if (!kap->pid || kap->pid == act->process->pid) {
        prp = act->process;
    } else {
        prp = lookup_pid_rcu(kap->pid);
        prp_rcu = 1;
        if (!prp) {
            rcu_read_unlock(&process_rcu);
            return EPERM;
        }
        if (prp != act->process) {
            rcu_read_unlock(&process_rcu);
            return EPERM;
        }
    }

    /* Verify the target thread exists. */
    if (tid != 0 && !(prp == act->process && tid == (int) (act->tid + 1))) {
        rcu_read_lock(&tthread_rcu);
        thp_rcu = 1;
        thp = vector_lookup(&prp->threads, (unsigned) (tid - 1));
        if (!thp || thp->tid != (uint32_t) (tid - 1)) {
            rcu_read_unlock(&tthread_rcu);
            if (prp_rcu)
                rcu_read_unlock(&process_rcu);
            return ESRCH;
        }
    }

    id = MAKE_CLOCKID(prp->pid, tid);

    if (thp_rcu)
        rcu_read_unlock(&tthread_rcu);
    if (prp_rcu)
        rcu_read_unlock(&process_rcu);

    /* KSTATUS write under the preempt fence. */
    KSTATUS(act) = id;

    return ENOERROR;
}
