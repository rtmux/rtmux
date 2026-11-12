/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <user-slice.h>

/* Create a channel (channel_create kcall).
 *
 * Allocates a channel_t (regular, async, or global), publishes it into the
 * owning process's channel vector (or the global directory), and records it as
 * the process's death channel when requested. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (flags, async maxbuf/bufsize, optional event/cred)
 * Return value: ENOERROR on success (chid in KSTATUS); an errno on failure.
 */
int kdecl ksys_channel_create(thread_t *act,
                              struct kcall_args_channel_create *kap)
{
    process_t *prp;
    channel_t *chp;
    int chid;
    soul_t *souls;
    vector_t *chvec;
    kspinlock_t *chvec_lock;
    int want_death_chp;
    rtmux_cap_t gate_cap = 0;
    struct sigevent event;
    struct _cred_info cred;
    struct user_slice cap_slice;

    /* Per-process vec_slock guards the chvec publication and the death_chp
     * write; per-channel chp->lock initialized before publish, then held for
     * the post-publish field writes that other CPUs can see via vector_lookup.
     * alloc_slock self-acquired by object_alloc.
     */

    prp = act->process;
    want_death_chp = (kap->flags & (RTMUX_FLG_CHF_THREAD_DEATH |
                                    RTMUX_FLG_CHF_COID_DISCONNECT)) != 0;

    /* Pre-flight unlocked check on death_chp — purely an early-out to avoid the
     * alloc. The authoritative re-check happens under prp->vec_slock below.
     * Without that re-check, two concurrent channel-create calls could both
     * pass here and race the death_chp write.
     */
    if (want_death_chp && prp->death_chp) {
        return EBUSY;
    }

    /* A capability gate only applies to a global channel: the gate protects
     * connect-by-chid, which is the global path. Reject the flag elsewhere so
     * the invariant "required_cap lives on a global_channel_t" always holds.
     */
    if ((kap->flags & RTMUX_FLG_CHF_CAP_GATED) &&
        !(kap->flags & RTMUX_FLG_CHF_GLOBAL)) {
        return EINVAL;
    }

    if (kap->flags & RTMUX_FLG_CHF_GLOBAL) {
        prp = NULL;
        souls = &global_channel_souls;
        chvec = &global_channel_vector;

        /* Range-check as well as probe, then take a kernel copy here rather
         * than reading the caller's memory again further down. The probe alone
         * only asks "is this accessible", which a mapped supervisor address
         * answers yes to, and both structures end up inside the channel object
         * the creator can read back: that made a kernel address an unbounded
         * disclosure rather than merely an unchecked read.
         *
         * The copy is not redundant with the probe. The publish path below
         * allocates a soul, takes the chvec lock, publishes through vector_add,
         * takes chp->lock and may mint a capability, all before it needs these
         * bytes; reading user memory at that point meant a sibling thread could
         * unmap the page and fault S-mode with two spinlocks held and no
         * handler armed, wedging every hart waiting on them. Copying while
         * nothing is held also closes the check-to-use window, so the bytes
         * stored are the bytes validated.
         */
        if (kap->event) {
            if (user_slice_get(act, &event, kap->event, sizeof(event)))
                return EFAULT;
        }
        if (kap->cred) {
            if (user_slice_get(act, &cred, kap->cred, sizeof(cred)))
                return EFAULT;
        }
        if (kap->flags & RTMUX_FLG_CHF_CAP_GATED) {
            if (!kap->cap_out)
                return EINVAL;

            /* Range-check AND probe here, before anything is allocated. The
             * probe used to live next to the writeback at the end of the
             * function; once it could return EFAULT rather than fault, failing
             * there meant the channel was already published in
             * global_channel_vector and the gating cap already minted, with
             * neither id ever reported to the caller. A loop of
             * channel_create(GLOBAL|CAP_GATED) with an unmapped cap_out would
             * then drain the channel souls and the endpoint cap table. Nothing
             * is held or allocated at this point, so an EFAULT here is free.
             */
            if (user_slice_init(act, kap->cap_out, sizeof(*kap->cap_out),
                                sizeof(*kap->cap_out), USER_SLICE_WR,
                                &cap_slice))
                return EFAULT;
        }
    } else {
        souls = &channel_souls;
        chvec = &prp->chancons;
    }

    /* Take the appropriate vector lock for the chvec mutations below:
     * prp->vec_slock for the per-process case, global_channel_slock for the
     * global path. Both are LOCK_CLASS_PROCVEC, distinct instances; only one is
     * ever held in this kcall.
     */
    chvec_lock = prp ? &prp->vec_slock : &global_channel_slock;
    kspinlock_lock(chvec_lock);

    /* Re-check death_chp under vec_slock (per-process case only — death_chp is
     * per-process state) — closes the TOCTOU between the pre-flight unlocked
     * check above and the write at the bottom. Without this, two concurrent
     * THREAD_DEATH channel creates on the same prp would both pass the
     * pre-flight, both alloc, both write death_chp, silently dropping the
     * loser's chp.
     */
    if (want_death_chp && prp && prp->death_chp) {
        kspinlock_unlock(chvec_lock);
        return EBUSY;
    }
    /* Allocate a channel entry. */
    if (!(chp = object_alloc(prp, souls))) {
        kspinlock_unlock(chvec_lock);
        return EAGAIN;
    }

    /* Per-channel lock — initialized before any field write so that a
     * concurrent observer that obtains chp via vector_add (below) sees a usable
     * lock. See struct comment in objects.h for ordering rules.
     */
    kspinlock_init(&chp->lock, LOCK_CLASS_CHANNEL, "channel_t.lock");

    chp->type = TYPE_CHANNEL;
    chp->process = prp;
    chp->destroyer = NULL;
    chp->flags = (uint16_t) kap->flags;
    chp->destroying = false;

    if (kap->flags & RTMUX_FLG_CHF_GLOBAL) {
        if (chp->reply_queue)
            crash();
    }

    /* Check if this is the remote IPC service message channel. Don't do this
     * until we know the channel's fully created, so we (mostly) don't have to
     * worry about remote_ipc.* fields not getting cleared on an error.
     */
    if (kap->flags & RTMUX_FLG_CHF_NET_MSG) {
        /* Registering the remote-IPC service channel is an IPC-namespace
         * authority, gated by RTMUX_PRIV_CAP_IPC rather than the root
         * credential; a loading child holds every cap transiently, so exclude
         * that window.
         */
        if ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
            !(act->process->priv_caps & RTMUX_PRIV_CAP_IPC)) {
            object_free(prp, &channel_souls, chp);
            kspinlock_unlock(chvec_lock);
            return EPERM;
        }
        if (remote_ipc.prp || remote_ipc.chp) {
            object_free(prp, &channel_souls, chp);
            kspinlock_unlock(chvec_lock);
            return EAGAIN;
        }
        remote_ipc.prp = prp;
        remote_ipc.chp = chp;
    }
    /* Add the channel to the channels vector. */
    chid = vector_add(chvec, chp, 1);
    if (chid == -1) {
        object_free(prp, &channel_souls, chp);
        if (kap->flags & RTMUX_FLG_CHF_NET_MSG) {
            remote_ipc.prp = NULL;
            remote_ipc.chp = NULL;
        }
        kspinlock_unlock(chvec_lock);
        return EAGAIN;
    }

    /* vector_add has now published "chp" — other CPUs can find it via
     * vector_lookup. Subsequent writes to per-channel state must be serialized
     * against any concurrent observer, so take chp->lock for the post-publish
     * mutations.
     */
    kspinlock_lock(&chp->lock);

    if (kap->flags &
        (RTMUX_FLG_CHF_THREAD_DEATH | RTMUX_FLG_CHF_COID_DISCONNECT)) {
        if (prp) { /* not global channel */
            prp->death_chp = chp;
        }
    }

    if (kap->flags & RTMUX_FLG_CHF_GLOBAL) {
        global_channel_t *global_channel = (global_channel_t *) chp;

        /* Public by default; the gated branch overwrites this. object_free
         * already zeroes recycled souls, but stating it keeps the public
         * invariant local.
         */
        global_channel->required_cap = 0;
        if (kap->flags & RTMUX_FLG_CHF_CAP_GATED) {
            /* Mint the gating cap over this channel in the creator's endpoint
             * table before publishing the global chid. The creator delegates it
             * to authorized clients via IPC cap transfer; connect admits a
             * client only if it holds a live cap over the channel object. Mint
             * while chid is still the raw vector index so unwind is clean.
             */
            if (rtmux_cap_alloc(&act->process->endpoint_caps, chp,
                                RTMUX_CAP_ENDPOINT_SEND,
                                &global_channel->required_cap) != EOK) {
                kspinlock_unlock(&chp->lock);
                vector_rem(chvec, (unsigned) chid);
                object_free(prp, souls, chp);
                kspinlock_unlock(chvec_lock);
                return EAGAIN;
            }
            gate_cap = global_channel->required_cap;
        }

        SET_GLOBAL_CHAN(chid);
        global_channel->event.sigev_notify = SIGEV_NONE;
        global_channel->max_num_buffer = kap->maxbuf;
        global_channel->buffer_size = kap->bufsize;
        if (kap->event) {
            global_channel->event = event;
            global_channel->ev_prp = act->process;
        }
        if (kap->cred) {
            global_channel->cred = cred;
        }
    }

    chp->chid = chid;

    kspinlock_unlock(&chp->lock);

    kspinlock_unlock(chvec_lock);

    /* Hand the minted gating cap id back to the creator outside the locks.
     *
     * Two checks, not one, because they defend different things. The entry
     * probe rejects a bad cap_out before anything is allocated, so the
     * deliberate attack -- create in a loop with an unmapped cap_out -- cannot
     * drain the channel souls or the cap table. This write is still fault-safe
     * because the entry probe is far away now: a sibling thread can unmap the
     * page during channel creation, and a raw store would take an unrecoverable
     * S-mode fault with no handler armed.
     *
     * Residual on that race: the channel is published and the caller sees
     * EFAULT, so the chid is orphaned. That needs the caller to unmap its own
     * output pointer mid-call, which is self-inflicted and not loopable, since
     * the deliberate case never gets this far.
     */
    if (kap->flags & RTMUX_FLG_CHF_CAP_GATED) {
        if (user_slice_copyout(&cap_slice, &gate_cap) != 0)
            return EFAULT;
    }

    SETKSTATUS(act, chid);
    return ENOERROR;
}

/* Revoke the gating caps of a global channel about to be freed.
 *
 * A gated channel's caps live in the endpoint tables of every process granted
 * connect authority (the creator plus every delegated client). Caps match on
 * the object pointer, so a stale cap over the freed channel could authorize a
 * connect to whatever object later reuses the soul. Revoke them everywhere
 * before the soul is recycled. Traversal mirrors process_clear_as_bindings():
 * process_rcu read side, cap_lock is a leaf taken per process, so no lock-rank
 * concern with a held chvec lock. No-op for public or non-global channels.
 */
static void global_channel_revoke_gates(channel_t *chp)
{
    int pidx;

    if (!(chp->flags & RTMUX_FLG_CHF_GLOBAL) ||
        ((global_channel_t *) chp)->required_cap == 0)
        return;

    rcu_read_lock(&process_rcu);
    for (pidx = 1; pidx < process_vector.nentries; ++pidx) {
        process_t *prp = vector_lookup(&process_vector, (unsigned) pidx);
        if (prp)
            (void) rtmux_cap_revoke_object(&prp->endpoint_caps, chp);
    }
    rcu_read_unlock(&process_rcu);
}


/* The destroy drains below must NOT abort a transfer that another hart has
 * claimed with RTMUX_ITF_MSG_DELIVERY. force_ready() returns 0 for such a
 * thread and records the abort in RTMUX_ITF_MSG_FORCE_RDY; the claimer (the
 * replier, sender or receiver holding the claim) then unlinks the thread and
 * completes or readies it itself. The drains re-read the queue head and retry
 * until the claimer has taken the thread off the queue.
 */


/* force_ready() returned 0: another hart holds a MSG_DELIVERY claim on thp and
 * will take it off the queue itself. Wait for the claim to drop with plain
 * loads, so the retry does not re-lock the channel (a test-and-set lock the
 * claimer needs for its tail) or re-IPI the claimer on every pass. The wait is
 * bounded; the caller re-reads the queue head either way. thp is protected by
 * the caller's tthread_rcu section.
 */
static void drain_wait_claim(thread_t *thp)
{
    for (unsigned spin = 0;
         spin < 4096 && (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY); spin++)
        __asm__ volatile("" ::: "memory");
}

/* One drain step: force_ready() thp and, if the abort was deferred to a
 * claimer, wait (bounded) for the claim to drop. The caller must keep thp valid
 * with a tthread_rcu section and re-reads the queue head afterwards.
 */
static void drain_force(thread_t *thp, unsigned err)
{
    if (!force_ready(thp, err))
        drain_wait_claim(thp);
}

/* _channel_destroy — internal variant with explicit process pointer. Used by
 * process teardown to destroy channels belonging to a dying process. 'act' is
 * still needed for KSYS_PREEMPT.
 */
int _channel_destroy(process_t *prp, int chid_arg)
{
    channel_t *chp;
    connect_t *cop;
    thread_t *thp;
    unsigned i;
    unsigned chid = (unsigned) chid_arg;
    soul_t *souls;
    vector_t *chvec;

    chvec = &prp->chancons;
    if (IS_GLOBAL_CHAN_SET(chid)) {
        CLEAR_GLOBAL_CHAN(chid);
        chvec = &global_channel_vector;
    }
    chp = vector_lookup2(chvec, chid);
    if (!chp || chp->type != TYPE_CHANNEL)
        return EINVAL;


    vector_flag(chvec, chid, 1);
    kspinlock_lock(&chp->lock);
    chp->destroying = true;
    kspinlock_unlock(&chp->lock);
    if ((prp == remote_ipc.prp) && (chp == remote_ipc.chp)) {
        remote_ipc.prp = NULL;
        remote_ipc.chp = NULL;
    }
    if (chp == prp->death_chp)
        prp->death_chp = NULL;
    if (chp == prp->sys_chp)
        prp->sys_chp = NULL;

    /* Drain the three channel queues under chp->lock. Lock-rank SCHED <
     * CHANNEL: drop chp->lock around force_ready (which self-locks on
     * chp->lock).
     *
     * Lifetime: each queued thp pointer is read here and then used after
     * chp->lock is dropped (for force_ready). A concurrent thread_destroy on
     * another hart may LINKPRIL_REM the same thp and object_free it in that
     * window. thread_destroy already rcu_sched_synchronize() before
     * object_free; each iteration therefore opens its own tthread_rcu read
     * section around the head read and the force_ready, so thp stays valid
     * while it is used and a retry loop never holds up a grace period.
     */
    for (;;) {
        rcu_read_lock(&tthread_rcu);
        kspinlock_lock(&chp->lock);
        thp = pril_first(&chp->send_queue);
        if (!thp) {
            kspinlock_unlock(&chp->lock);
            rcu_read_unlock(&tthread_rcu);
            break;
        }
        switch (TYPE_MASK(thp->type)) {
        case TYPE_NOTIFICATION:
        case TYPE_VNOTIFICATION:
            pril_rem(&chp->send_queue, thp);
            object_free(chp->process, &notification_souls, thp);
            kspinlock_unlock(&chp->lock);
            break;
        case TYPE_THREAD:
            kspinlock_unlock(&chp->lock);
            drain_force(thp, ESRCH);
            break;
        default:
            kspinlock_unlock(&chp->lock);
            rcu_read_unlock(&tthread_rcu);
            goto send_done;
        }
        rcu_read_unlock(&tthread_rcu);
    }
send_done:

    /* Read the queue head under chp->lock to serialize against a concurrent
     * ksys_msg_receivev's LINKPRIL_BEG on chp->receive_queue. Drop the lock
     * around force_ready (sched_slock < CHANNEL rank).
     */
    for (;;) {
        rcu_read_lock(&tthread_rcu);
        kspinlock_lock(&chp->lock);
        thp = chp->receive_queue;
        kspinlock_unlock(&chp->lock);
        if (!thp) {
            rcu_read_unlock(&tthread_rcu);
            break;
        }

        /* Defensive guard: a freed (0x6d-poisoned) thread can be left dangling
         * on receive_queue by a missing unlink-before-free; force_ready'ing it
         * would crash with "force_ready BAD TYPE". Mirror the reply_queue drain
         * below, which breaks on a non-thread head.
         */
        if (TYPE_MASK(thp->type) != TYPE_THREAD) {
            rcu_read_unlock(&tthread_rcu);
            break;
        }
        drain_force(thp, ESRCH);
        rcu_read_unlock(&tthread_rcu);
    }

    if (!(chp->flags & RTMUX_FLG_CHF_GLOBAL)) {
        for (;;) {
            rcu_read_lock(&tthread_rcu);
            kspinlock_lock(&chp->lock);
            thp = chp->reply_queue;
            kspinlock_unlock(&chp->lock);
            if (!thp || TYPE_MASK(thp->type) != TYPE_THREAD) {
                rcu_read_unlock(&tthread_rcu);
                break;
            }
            drain_force(thp, ESRCH | _FORCE_SET_ERROR | _FORCE_NO_UNBLOCK);
            rcu_read_unlock(&tthread_rcu);
        }
    }

    if (!(chp->flags & RTMUX_FLG_CHF_GLOBAL)) {
        for (i = 0; i < prp->chancons.nentries; ++i) {
            if ((cop = VECP2(cop, &prp->chancons, i))) {
                if (cop->type == TYPE_CONNECTION && cop->channel == chp &&
                    cop->scoid == i) {
                    vector_rem(&prp->chancons, i);
                    if (cop->links == 0) {
                        connect_soul_free(prp, cop);
                    } else {
                        do {
                            if (cop->process && cop->process->death_chp)
                                connect_coid_disconnect(cop,
                                                        cop->process->death_chp,
                                                        prp->terming_priority);
                            cop->channel = NULL;
                        } while ((cop = cop->next));
                    }
                }
            }
        }
    } else {
        cop = ((global_channel_t *) chp)->cons;
        while (cop) {
            connect_t *next = cop->next;
            cop->channel = NULL;
            cop->next = NULL;
            cop = next;
        }
    }

    if (!(chp = vector_rem(chvec, chid)))
        return EINVAL;

    if (chp->flags & RTMUX_FLG_CHF_GLOBAL)
        souls = &global_channel_souls;
    else
        souls = &channel_souls;

    /* Revoke the gating caps before the soul can be reused (chp is already
     * unpublished, so no new connect can find it).
     */
    global_channel_revoke_gates(chp);

    /* RCU drain. chp is now invisible to new lookups (vector_rem
     * + earlier vector_flag); wait for any in-flight connobj_rcu reader holding
     * a stale chp pointer to exit before recycling.
     */
    rcu_sched_synchronize();
    object_free(prp, souls, chp);

    return EOK;
}

/* Destroy a channel and unblock everything queued on it (channel_destroy
 * kcall).
 *
 * Marks the channel unavailable in its directory, drains the send / receive /
 * reply queues (waking blocked threads with ESRCH), NULLs out server
 * connections' channel pointers, and frees the channel. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->chid is the channel to destroy
 * Return value: EOK on success; EINVAL if the chid is not a live channel.
 */
int kdecl ksys_channel_destroy(thread_t *act,
                               struct kcall_args_channel_destroy *kap)
{
    process_t *prp;
    channel_t *chp;
    connect_t *cop;
    thread_t *thp;
    unsigned i;
    unsigned chid;
    soul_t *souls;
    vector_t *chvec;
    kspinlock_t *chvec_lock;

    /* Vector exclusion goes through prp->vec_slock (per-process chancons) or
     * global_channel_slock (global global_channel_vector); same
     * LOCK_CLASS_PROCVEC, distinct instances. Channel-queue mutations under
     * chp->lock.
     */

    prp = act->process;

    chid = (unsigned) kap->chid;
    chvec = &prp->chancons;
    chvec_lock = &prp->vec_slock;
    if (IS_GLOBAL_CHAN_SET(chid)) {
        CLEAR_GLOBAL_CHAN(chid);
        chvec = &global_channel_vector;
        chvec_lock = &global_channel_slock;
    }

    /* vector_lookup2 + vector_flag(chvec, chid, 1) must be atomic w.r.t. a
     * concurrent connect_attach searching for chp via chvec — if attach finds
     * chp before our flag mark and we then destroy chp under it, attach would
     * crash on a freed channel. Both paths now covered by chvec_lock.
     */
    kspinlock_lock(chvec_lock);

    /* Make sure the channel is valid. Allow lookup of already-flagged channel
     * on restarted system calls.
     */
    chp = vector_lookup2(chvec, chid);
    if (!chp || chp->type != TYPE_CHANNEL) {
        kspinlock_unlock(chvec_lock);
        return EINVAL;
    }

    /* vector_lookup2 returns the channel whether or not it is already flagged
     * for destruction: the destroy flag is bit 1 (vector_flag sets bit value
     * 2), while lookup2 only suppresses freed bit-0 slots. Read the raw slot to
     * distinguish a fresh destroy from one already in progress — only the
     * thread that began it, now re-running via KCALL_RESTART, may resume; a
     * concurrent ChannelDestroy on the same chid must not also enter teardown
     * and drain the shared queues underneath it.
     */
    if (((uintptr_t) VEC(chvec, chid) & 2) != 0) {
        if (chp->destroyer != act) {
            kspinlock_unlock(chvec_lock);
            return EINVAL;
        }
    } else {
        chp->destroyer = act; /* claim on the first, unflagged pass */
    }

    /* Mark channel as unavailable. */
    vector_flag(chvec, chid, 1);
    kspinlock_unlock(chvec_lock);
    kspinlock_lock(&chp->lock);
    chp->destroying = true;
    kspinlock_unlock(&chp->lock);
    /* Check if the remote IPC service is releasing its message channel. */
    if ((prp == remote_ipc.prp) && (chp == remote_ipc.chp)) {
        static unsigned retries;

        for (i = 0; i < vthread_vector.nentries; ++i) {
            vthread_t *vthp;

            vthp = vector_rem(&vthread_vector, i);
            if (vthp) {
                unsigned saved_flags;
                saved_flags = vthp->flags;
                atomic_clr(&vthp->flags, RTMUX_FLG_THR_KILLSELF);
                if (!force_ready((thread_t *) (void *) vthp,
                                 EINTR | _FORCE_SET_ERROR)) {
                    /* We couldn't force_ready the thread, we must have been in
                     * the middle of a CONFIG_SMP_MSGOPT message pass. The
                     * force_ready() call would have invoked
                     * force_ready_msgxfer() to abort the transfer, so let's get
                     * out of the kernel and let that be processed. If we
                     * restart 1 million times without getting out of here,
                     * something's gone wrong....
                     */
                    if (++retries == 1000000)
                        crash();
                    vthp->flags = saved_flags;
                    vector_add(&vthread_vector, vthp, i);
                    KCALL_RESTART(act);
                    return ENOERROR;
                }
                object_free(NULL, &vthread_souls, vthp);
            }
            KSYS_PREEMPT(act, ENOERROR);
        }
        retries = 0;
        remote_ipc.prp = NULL;
        remote_ipc.chp = NULL;
    }
    /* Check for destruction of the death channel. */
    if (chp == prp->death_chp) {
        prp->death_chp = NULL;
    }
    if (chp == prp->sys_chp) {
        prp->sys_chp = NULL;
    }

    /* Drain the three channel queues under chp->lock. Lock dropped around
     * force_ready / remote_ipc_send (both self-lock chp->lock). Unblock all
     * threads send blocked on the channel We have to do this before we NULL the
     * cop->channel pointers below so that force_ready can find the appropriate
     * PRIL_HEAD structure.
     */
    kspinlock_lock(&chp->lock);
    while ((thp = pril_first(&chp->send_queue))) {
        switch (TYPE_MASK(thp->type)) {
        case TYPE_NOTIFICATION:
        case TYPE_VNOTIFICATION:
            pril_rem(&chp->send_queue, thp);
            object_free(chp->process, &notification_souls, thp);
            break;
        case TYPE_VTHREAD:
            kspinlock_unlock(&chp->lock);

            /* Turn on RTMUX_FLG_THR_KCALL_ERR_SET so that if remote_ipc_send()
             * calls kcall_err(), it won't advance the IP - we're going to be
             * restarting the kernel call so that the remote IPC service can
             * process the notification
             */
            atomic_or(&act->flags, RTMUX_FLG_THR_KCALL_ERR_SET);
            (void) remote_ipc_send((kcall_args_t *) (void *) kap,
                                   thp->un.remote.vtid, thp->blocked_on, thp);
            KCALL_RESTART(act);
            return ENOERROR;
        case TYPE_THREAD:
            /* thp is still queued under chp->lock, so it is valid; open the RCU
             * section before dropping the lock (see _channel_destroy).
             */
            rcu_read_lock(&tthread_rcu);
            kspinlock_unlock(&chp->lock);
            drain_force(thp, ESRCH);
            rcu_read_unlock(&tthread_rcu);
            kspinlock_lock(&chp->lock);
            break;
        default:
            kspinlock_unlock(&chp->lock);
            crash();
            return EINVAL; /* NOTREACHED */
        }
        KSYS_PREEMPT(act, ENOERROR);
    }
    kspinlock_unlock(&chp->lock);

    /* Unblock all threads receive blocked on the channel receive_queue head
     * read under chp->lock to serialize against a concurrent
     * ksys_msg_receivev's LINKPRIL_BEG.
     */
    for (;;) {
        /* Per-iteration tthread_rcu section so a concurrent thread_destroy
         * can't object_free this thp between our head-read and force_ready (see
         * _channel_destroy). Exit BEFORE KSYS_PREEMPT (which may return), so
         * the section never leaks.
         */
        rcu_read_lock(&tthread_rcu);
        kspinlock_lock(&chp->lock);
        thp = chp->receive_queue;
        kspinlock_unlock(&chp->lock);
        if (!thp) {
            rcu_read_unlock(&tthread_rcu);
            break;
        }
        CRASHCHECK((TYPE_MASK(thp->type) != TYPE_THREAD) &&
                   (TYPE_MASK(thp->type) != TYPE_VTHREAD));
        drain_force(thp, ESRCH);
        rcu_read_unlock(&tthread_rcu);
        KSYS_PREEMPT(act, ENOERROR);
    }

    if (!(chp->flags & RTMUX_FLG_CHF_GLOBAL)) {
        /* Unblock all threads reply blocked on the channel reply_queue head
         * read under chp->lock.
         */
        for (;;) {
            /* Per-iteration tthread_rcu section (see the receive loop above).
             * Exit before every return / KSYS_PREEMPT.
             */
            rcu_read_lock(&tthread_rcu);
            kspinlock_lock(&chp->lock);
            thp = chp->reply_queue;
            kspinlock_unlock(&chp->lock);
            if (!thp) {
                rcu_read_unlock(&tthread_rcu);
                break;
            }
            switch (TYPE_MASK(thp->type)) {
            case TYPE_VTHREAD:
                /* Turn on RTMUX_FLG_THR_KCALL_ERR_SET so that if
                 * remote_ipc_send() calls kcall_err(), it won't advance the IP
                 * - we're going to be restarting the kernel call so that the
                 * remote IPC service can process the notification
                 */
                atomic_or(&act->flags, RTMUX_FLG_THR_KCALL_ERR_SET);
                (void) remote_ipc_send((kcall_args_t *) (void *) kap,
                                       thp->un.remote.vtid, thp->blocked_on,
                                       thp);
                rcu_read_unlock(&tthread_rcu);
                KCALL_RESTART(act);
                return ENOERROR;
            case TYPE_THREAD:
                /* Disable unblock notification since we don't want to add more
                 * stuff to this channel that we're in the process of
                 * destroying...
                 */
                drain_force(thp, ESRCH | _FORCE_SET_ERROR | _FORCE_NO_UNBLOCK);
                break;
            default:
                crash();
            }
            rcu_read_unlock(&tthread_rcu);
            KSYS_PREEMPT(act, ENOERROR);
        }
    } else {
        /* release all packages/send requests */
        if (chp->flags & RTMUX_FLG_CHF_GLOBAL) {
            struct global_msg_entry *global_msg;
            global_channel_t *global_channel = (global_channel_t *) chp;

            if (global_channel->free) {
                _sfree(global_channel->free,
                       global_channel->buffer_size + sizeof(*global_msg));
            }

            /* global_msg list is the chp->reply_queue field cast to a different
             * type; same head pointer. Wrap the head-pop in chp->lock for
             * consistency with the regular reply_queue case above.
             */
            kspinlock_lock(&chp->lock);
            while (
                (global_msg = (struct global_msg_entry *) chp->reply_queue)) {
                chp->reply_queue = (void *) global_msg->next;
                kspinlock_unlock(&chp->lock);
                _sfree(global_msg,
                       global_channel->buffer_size + sizeof(*global_msg));
                kspinlock_lock(&chp->lock);
            }
            kspinlock_unlock(&chp->lock);
        }
    }

    /* Find all server connections and mark their channels as NULL. */
    if (!(chp->flags & RTMUX_FLG_CHF_GLOBAL)) {
        for (i = 0; i < prp->chancons.nentries; ++i) {
            if ((cop = VECP2(cop, &prp->chancons, i))) {
                if (cop->type == TYPE_CONNECTION && cop->channel == chp &&
                    cop->scoid == i) {
                    /* Remove the server connection from the servers channels
                     * vector.
                     */
                    vector_rem(&prp->chancons, i);

                    if (cop->links == 0) {
                        /* Server connection unpublished from chancons; recycle
                         * it.
                         */
                        connect_soul_free(prp, cop);
                    } else {
                        do {
                            if (cop->process && cop->process->death_chp) {
                                connect_coid_disconnect(cop,
                                                        cop->process->death_chp,
                                                        act->priority);
                            }
                            /* Null channel pointer since it is gone. */
                            cop->channel = NULL;
                        } while ((cop = cop->next));
                    }
                }
            }
            KSYS_PREEMPT(act, ENOERROR);
        }
    } else {
        cop = ((global_channel_t *) chp)->cons;
        while (cop) {
            connect_t *next = cop->next;
            cop->channel = NULL;
            cop->next = NULL;
            cop = next;
        }
    }

    /* Final removal of chp from the directory. Re-take chvec_lock for the
     * vector_rem; since we already vector_flag'd at the top, this is the second
     * half of the publish/unpublish atomic pair.
     */
    kspinlock_lock(chvec_lock);
    chp = vector_rem(chvec, chid);
    kspinlock_unlock(chvec_lock);
    if (!chp) {
        return EINVAL;
    }

    if (chp->flags & RTMUX_FLG_CHF_GLOBAL)
        souls = &global_channel_souls;
    else
        souls = &channel_souls;

    /* Revoke the gating caps before the soul can be reused (chp is already
     * unpublished, so no new connect can find it).
     */
    global_channel_revoke_gates(chp);

    /* RCU drain — chp is now invisible (vector_rem + earlier vector_flag). Wait
     * for any in-flight connobj_rcu reader still holding chp before recycling
     * its memory.
     */
    rcu_sched_synchronize();
    /* Release the channel entry. */
    object_free(prp, souls, chp);

    return EOK;
}
