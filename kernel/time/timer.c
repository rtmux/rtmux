/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <user-slice.h>

#define VALID_CLOCKID(kap) \
    switch ((kap)->id) {   \
    case CLOCK_MONOTONIC:  \
    case CLOCK_REALTIME:   \
    case CLOCK_SOFTTIME:   \
        break;             \
    default:               \
        return EINVAL;     \
    }

/* Timer territory. Live kcalls: 70-75 (alarm = ENOSYS).
 *
 * Shared state:
 *   - prp->timers (per-process vector_t) — protected by prp->vec_slock
 *     for create/destroy; settime/info read under preempt fence.
 *   - tip->{flags, itime, thread, event} — per-ktimer_t; tip->lock
 *     (LOCK_CLASS_TIMER) initialized in timer_alloc.
 *   - Active-timer wheel — managed by timer_activate/deactivate
 *     internals; self-locking via the timer subsystem.
 *
 * Cross-CPU residual: vector_add/vector_rem on prp->timers without vec_slock,
 * so concurrent __timer_create / timer_destroy on the same process from
 * different threads/CPUs is a race surface. Practical risk is low (timers per
 * process are few; create/destroy is infrequent).
 */

/* #70 __timer_create. */
int kdecl ksys_timer_create(thread_t *act, struct kcall_args_timer_create *kap)
{
    process_t *prp;
    ktimer_t *tip;
    int id;
    struct sigevent ev;

    VALID_CLOCKID(kap);

    if (kap->event && user_slice_get(act, &ev, kap->event, sizeof(ev)))
        return EFAULT;

    prp = act->process;

    /* Allocate a timer entry. */
    if (!(tip = timer_alloc(prp))) {
        return EAGAIN;
    }

    tip->clockid = (uint8_t) kap->id;
    if (kap->event) {
        tip->event = ev;

        if (SIGEV_GET_TYPE(&tip->event) == SIGEV_NOTIFICATION) {
            connect_t *cop;
            channel_t *chp;

            /* Open a connobj_rcu read section before lookup_connect so the
             * cop/chp we deref below cannot be freed by a concurrent
             * _channel_destroy (which rcu_sched_synchronize()s before
             * object_free). Mirrors ksys_msg_sendnotification; trap-exit
             * lookup_rcvid_release closes the nested guard, including on the
             * EBADF/ENOERROR returns below.
             */
            lookup_rcvid_open();

            /* Endpoint send right is the notification authority. */
            if (!(cop = lookup_connect((unsigned) tip->event.sigev_coid)) ||
                cop->type != TYPE_CONNECTION ||
                (__atomic_load_n(&cop->rights, __ATOMIC_RELAXED) &
                 RTMUX_CAP_ENDPOINT_SEND) == 0 ||
                !(chp = cop->channel)) {
                timer_free(prp, tip);
                return EBADF;
            }

            /* Reserve one notification soul so timer-expiry delivery from the
             * fire path is allocation-free (§15). Fail the create rather than
             * risk a dropped wakeup under memory pressure later.
             */
            if (!object_reserve(&notification_souls)) {
                timer_free(prp, tip);
                return EAGAIN;
            }
            tip->nresv = 1;
        }
    } else {
        /* No event specified — reject (signals removed, must use
         * SIGEV_NOTIFICATION)
         */
        timer_free(prp, tip);
        return EINVAL;
    }

    /* Reject signal-based timer events — use SIGEV_NOTIFICATION.
     *
     * SIGEV_THREAD is also rejected: the fire path runs from trap context
     * (STIMER) via sigevent_deliver_inline, which cannot safely call
     * thread_create. Callers wanting per-fire thread spawn must use
     * SIGEV_NOTIFICATION to a channel and have a user-mode thread msg_receive
     * on it.
     */
    switch (SIGEV_GET_TYPE(&tip->event)) {
    case SIGEV_SIGNAL:
    case SIGEV_SIGNAL_THREAD:
    case SIGEV_THREAD:
        timer_free(prp, tip);
        return EINVAL;
    default:
        break;
    }

    tip->flags &= (uint8_t) ~RTMUX_TI_ACTIVE;
    tip->flags |= RTMUX_TI_TARGET_PROCESS;
    /* Add the channel to the process's timers vector. */
    if ((id = vector_add(&prp->timers, tip, 0)) == -1) {
        timer_free(prp, tip);
        return EAGAIN;
    }

    if (!kap->event) {
        /* POSIX says that with a NULL sigevent, the value field of the signal
         * should be the timer id.
         */
        tip->event.sigev_value.sival_int = id;
    }

    /* Put the timer id in the top 16 bits of the notify type It will be used in
     * intrevent_drain() to adjust overruns interevent_exe() will only look at
     * the lower 16 bits.
     */
    tip->event.sigev_notify += (id + 1) << 16;

    SETKSTATUS(act, id);
    return ENOERROR;
}


/* _timer_destroy — internal variant with explicit process pointer. Called under
 * the kernel lock (via syscall_priv).
 */
int _timer_destroy(process_t *prp, int id)
{
    ktimer_t *tip;

    if (!(tip = vector_rem(&prp->timers, (unsigned) id)))
        return EINVAL;
    timer_deactivate(tip);
    timer_free(prp, tip);
    return EOK;
}

/* #71 timer_destroy. */
int kdecl ksys_timer_destroy(thread_t *act,
                             struct kcall_args_timer_destroy *kap)
{
    process_t *prp;
    ktimer_t *tip;
    int id = kap->id;

    prp = act->process;

    /* Remove the timer from the process's timer vector. */
    if (!(tip = vector_rem(&prp->timers, (unsigned) id))) {
        return EINVAL;
    }
    /* If timer active we must unlink it from the list of active timers. */
    timer_deactivate(tip);

    /* Release the timer entry. */
    timer_free(prp, tip);

    return EOK;
}


/* #72 __timer_settime. tip lookup via prp->timers (residual: vec_slock not held
 * — see the territory banner). tip->lock would protect the field mutations on
 * tip; not yet wired through the timer subsystem helpers.
 */
int kdecl ksys_timer_settime(thread_t *act,
                             struct kcall_args_timer_settime *kap)
{
    ktimer_t *tip;
    struct _itimer left, in;
    struct user_slice old_slice;

    left.nsec = 0;
    left.interval_nsec = 0;


    /* Make sure timer is valid. */
    if ((tip = vector_lookup(&act->process->timers, (unsigned) kap->id)) ==
        NULL) {
        return EINVAL;
    }
    if (user_slice_get(act, &in, kap->itime, sizeof(in)))
        return EFAULT;

    if (kap->oitime) {
        if (user_slice_init(act, kap->oitime, sizeof(left), sizeof(left),
                            USER_SLICE_WR, &old_slice))
            return EFAULT;
        timer_remaining(tip, &left);
    }


    /* If the timer is active we must unlink it from the list of active timers.
     * To let the user know if a real event was queued, we adjust the time left.
     * If nsec is zero, we guarantee the event was queued.
     */
    if (!timer_deactivate(tip)) {
        left.nsec = 0;
    } else if (!left.nsec) {
        left.nsec = 1;
    }

    /* Check for timer disable (nsec == 0). */
    if (in.nsec != 0) {
        tip->flags &= (uint8_t) ~RTMUX_TI_TOD_BASED;
        if (kap->flags & TIMER_ABSTIME) {
            tip->flags |= RTMUX_TI_ABSOLUTE;
            tip->itime.nsec = in.nsec;
            if (tip->clockid != CLOCK_MONOTONIC) {
                tip->flags |= RTMUX_TI_TOD_BASED;
            }
        } else {
            tip->flags &= (uint8_t) ~(RTMUX_TI_ABSOLUTE | RTMUX_TI_TOD_BASED);
            SNAP_TIME_INLINE(tip->itime.nsec, 0);

            /* Exact relative deadline — no tick roundup; the rearm path fires
             * at the earlier of the next periodic tick and the earliest active
             * timer.
             */
            tip->itime.nsec += in.nsec;
        }

        /* Save interval of the timer (repeat rate). This may be zero. */
        tip->itime.interval_nsec = in.interval_nsec;
        tip->flags &= (uint8_t) ~RTMUX_TI_EXPIRED;

        /* Set the thread to use for delivery */
        tip->thread = act;

        timer_activate(tip);
    }

    if (kap->oitime && user_slice_copyout(&old_slice, &left))
        return EFAULT;

    return EOK;
}


/* #73 timer_info — read tip fields, optionally clear overruns. */
int kdecl ksys_timer_info(thread_t *act, struct kcall_args_timer_info *kap)
{
    ktimer_t *tip;
    process_t *prp;
    unsigned id;
    struct _timer_info *inp = kap->info;
    struct _timer_info info;
    int have_info = 0;
    int status = EOK;

    if (kap->pid == 0 || kap->pid == act->process->pid) {
        rcu_read_lock(&process_rcu);
        prp = act->process;
    } else {
        prp = lookup_pid_rcu(kap->pid);
    }
    if (prp != act->process) {
        status = EPERM;
        goto out_unlock;
    }

    /* Make sure timer is valid. */
    id = (unsigned) kap->id;
    if (kap->flags & RTMUX_TIMER_SEARCH) {
        tip = vector_search(&prp->timers, id, &id);
    } else {
        tip = vector_lookup2(&prp->timers, id);
    }
    if (!tip) {
        status = EINVAL;
        goto out_unlock;
    }

    if (inp) {
        if (!WITHIN_BOUNDARY((uintptr_t) inp, (uintptr_t) inp + sizeof(*inp),
                             act->process->boundary_addr)) {
            status = EFAULT;
            goto out_unlock;
        }

        /* xfer_probe directly, not WR_PROBE_INT: the macro's embedded return
         * would skip out_unlock, and this runs inside the RCU read section
         * opened above. The bounds failure two lines up already goes there.
         *
         * Probe the whole struct, not just otime: the write below stores all of
         * *inp, so probing one member left the rest of the extent unchecked.
         * The store itself is fault-safe (see out_unlock) because it sits far
         * from this probe (the caller can unmap the page in between), so this
         * probe is the early reject for the deliberate case, not the guarantee.
         */
        if (xfer_probe(inp, sizeof(*inp) / sizeof(int), 1) != 0) {
            status = EFAULT;
            goto out_unlock;
        }
        memset(&info, 0, sizeof(info));
        info.event = tip->event;
        info.event.sigev_notify &=
            0xffff; /* Turn off timer id in top 16 bits */
        info.notify = info.event.sigev_notify;
        info.flags = tip->flags;
        info.overruns = tip->overruns;
        if (tip->flags & RTMUX_TI_TARGET_PROCESS) {
            info.tid = 0;
        } else {
            info.tid = (int32_t) (tip->thread->tid + 1); /* 1-based POSIX tid */
        }
        info.clockid = tip->clockid;
        info.itime = tip->itime;
        timer_remaining(tip, &info.otime);
        have_info = 1;
    }

    /* Copy out before the reset, and inside the RCU section, so the clear is
     * conditional on the caller actually receiving the count.
     *
     * Fault-safe, not `*inp = info`: the probe above cannot promise the page
     * survives to the store, and this kcall arms no fault handler, so a raw
     * store was an unrecoverable S-mode fault. xfer_memcpy does not block, so
     * it is safe inside the read section; it recovers into its own setjmp and
     * returns, which is why it can sit here rather than after the unlock.
     *
     * Ordering matters for RTMUX_TIMER_RESET_OVERRUNS specifically. With the
     * clear ahead of the copy, a copy that faulted destroyed the count in the
     * kernel and never delivered it, so a retrying caller read 0 and silently
     * under-reported missed expirations forever. tip is only valid inside this
     * section, so restoring it after the unlock is not an option; doing the
     * copy first is.
     */
    if (have_info && xfer_memcpy(inp, &info, sizeof(info)) != 0) {
        status = EFAULT;
        goto out_unlock;
    }

    if (kap->flags & RTMUX_TIMER_RESET_OVERRUNS) {
        tip->overruns = 0;
    }

out_unlock:
    rcu_read_unlock(&process_rcu);
    if (status != EOK)
        return status;

    SETKSTATUS(act, id);
    return ENOERROR;
}


/* #75 __timer_timeout — set per-thread timeout for upcoming syscall blocking;
 * if NANOSLEEP requested, block here.
 *
 * Per-thread state mutated: act->{timeout_flags, timeout, args.to.timeptr,
 * state}. All single-writer-by-CPU since act IS the running thread on this CPU.
 */
int kdecl ksys_timer_timeout(thread_t *act,
                             struct kcall_args_timer_timeout *kap)
{
    ktimer_t *tip;
    unsigned tls_flags;
    int id = (int) kap->id; /* kap->id may be corrupted by SETKSTATUS */
    uint64_t ntime;
    struct sigevent ev;
    struct user_slice otime_slice;

    VALID_CLOCKID(kap);

    if (kap->ntime && user_slice_get(act, &ntime, kap->ntime, sizeof(ntime)))
        return EFAULT;
    if (kap->event && user_slice_get(act, &ev, kap->event, sizeof(ev)))
        return EFAULT;

    /* Validate only: the pointer is kept in act->args.to.timeptr and written
     * back at special-return through xfer_memcpy
     * (kernel/thread/special-return.c)
     */
    if ((kap->timeout_flags & (1 << STATE_NANOSLEEP)) && kap->otime &&
        user_slice_init(act, kap->otime, sizeof(*kap->otime),
                        sizeof(*kap->otime), USER_SLICE_WR, &otime_slice))
        return EFAULT;

    tls_flags = TLS_FLAGS_SAFE(act);


    /* Return prev flags and load new flags. */
    SETKSTATUS(act, act->timeout_flags);
    act->timeout_flags = kap->timeout_flags & RTMUX_FLG_TIMEOUT_MASK;

    /* If no ntime is provided they don't want to block at all. */
    if (!kap->ntime || timer_past(id, (int) kap->timeout_flags, &ntime))
        act->timeout_flags |= RTMUX_FLG_TIMEOUT_IMMEDIATE;

    /* Is there already a timeout timer inplace? If so replace it. */
    if ((tip = act->timeout)) {
        if (tip->flags & RTMUX_TI_ACTIVE)
            crash();
        if (kap->timeout_flags == 0) {
            /* Delete the timer. */
            act->timeout = NULL;
            timer_free(act->process, tip);
            return ENOERROR;
        }
    } else if (kap->timeout_flags) {
        if (!(act->timeout = tip = timer_alloc(act->process))) {
            return EAGAIN;
        }
    } else {
        return ENOERROR;
    }

    tip->clockid = (uint8_t) id;
    tip->thread = act;
    if (!kap->event) {
        tip->event.sigev_notify = SIGEV_UNBLOCK;
    } else {
        tip->event = ev;
    }
    tip->itime.interval_nsec = 0;
    if (kap->ntime) {
        tip->flags &= (uint8_t) ~RTMUX_TI_TOD_BASED;
        if (kap->timeout_flags & (uint32_t) TIMER_ABSTIME) {
            tip->flags |= RTMUX_TI_ABSOLUTE;
            tip->itime.nsec = ntime;
            if (tip->clockid != CLOCK_MONOTONIC)
                tip->flags |= RTMUX_TI_TOD_BASED;
        } else {
            tip->flags &= (uint8_t) ~(RTMUX_TI_ABSOLUTE | RTMUX_TI_TOD_BASED);
            SNAP_TIME_INLINE(tip->itime.nsec, 0);

            /* Exact relative deadline (see __timer_create) — no tick roundup.
             */
            tip->itime.nsec += ntime;
        }
    }

    /* Must remember address to save time left if a signal hits us. */
    if (act->timeout_flags & (1 << STATE_NANOSLEEP)) {
        if (PENDCAN(tls_flags)) {
            SETKIP_FUNC(act, act->process->canstub);
            return ENOERROR;
        }

        act->args.to.timeptr = kap->otime; /* Save return time ptr. */
        SETKSTATUS(act, 0);
        act->state = STATE_NANOSLEEP;
        _TRACE_TH_EMIT_STATE(act, NANOSLEEP);
        /* block() is sched_slock-internal; drop preempt fence first. */
        block();
        return ENOERROR;
    }
    return ENOERROR;
}
