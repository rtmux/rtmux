/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <core/globals.h>
#include <user-slice.h>
/* #define   MSG_XFER_CHUNK_SIZE     1024 */


/* Establish the calling thread as the server for a given rcvid (msg_current
 * kcall).
 *
 * Binds act as the server of the reply-blocked client thread named by
 * kap->rcvid, disowning any prior client, and propagates the client's priority
 * to act (priority inheritance) unless the channel is fixed-priority.
 * Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->rcvid identifies the client thread
 * Return value: EOK on success; ESRCH if the rcvid no longer names a
 *               reply-blocked client.
 */
int kdecl ksys_msg_current(thread_t *act, struct kcall_args_msg_current *kap)
{
    connect_t *cop;
    thread_t *thp;
    int do_pi = 0;
    uint8_t new_prio = 0;
    kdispatch_t *new_dpp = NULL;


    /* Disown act's prior client, if any. Single-pointer atomic writes; the
     * prior client briefly observes server=0 with act->client cleared —
     * eventually-consistent for the prior client's own ksys_msg_current. A
     * per-thread_t lock would close this window; not yet instantiated
     * (LOCK_CLASS_THREAD placeholder).
     */
    if (act->client != 0) {
        act->client->args.ms.server = 0;
        act->client = 0;
    }

    if (kap->rcvid == 0) {
        return EOK;
    }

    /* lookup_rcvid runs unlocked: the chancons/threads vec_lookups share the
     * helper-level vec_slock race surface common to every IPC kcall.
     */
    if ((cop = lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) ==
        NULL) {
        return ENOERROR;
    }

    /* Make sure thread is replied blocked on a channel owned by this process.
     */
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        return ESRCH;
    }

    /* Verify that the message has been fully received, and that the receiving
     * thread has completed any specialret() processing that needs to be done.
     */
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        return ESRCH;
    }

    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;

        /* NULL: client's blocked_on was cleared by a concurrent teardown
         * (force_ready on a client whose process is exiting) -> ESRCH, matching
         * the locked recheck and the guard in fastmsg.c. Without it the deref
         * below load-faults near NULL.
         */
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
            return ESRCH;
        }
    }

    /* Take cop->lock (LOCK_CLASS_CONNECT) on the *final* cop — i.e.
     * thp->blocked_on after the VCONNECT swap. Re-verify the state predicates
     * under the lock (drop-and-recheck): a concurrent ksys_msg_replyv /
     * ksys_msg_error / force_ready on another CPU may have moved thp out of
     * STATE_REPLY between the unlocked checks above and the locked section.
     */
    kspinlock_lock(&cop->lock);
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        kspinlock_unlock(&cop->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        kspinlock_unlock(&cop->lock);
        return ESRCH;
    }
    if (thp->blocked_on != cop) {
        kspinlock_unlock(&cop->lock);
        return ESRCH;
    }

    if (thp->args.ms.server != 0) {
        thp->args.ms.server->client = 0;
    }

    thp->args.ms.server = act;
    act->client = thp;

    /* Snapshot the PI inputs while holding cop->lock; cop->lock must be dropped
     * before adjust_priority() because adjust_priority acquires sched_slock
     * (SCHED), and SCHED < CONNECT — the rank rule forbids holding cop->lock
     * while taking sched_slock.
     */
    if ((cop->channel->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
        do_pi = 1;
        new_prio = thp->priority;
        new_dpp = thp->dpp;
    }

    kspinlock_unlock(&cop->lock);

    /* PI propagation — outside cop->lock to honor SCHED-vs-CONNECT rank. */
    if (do_pi) {
        act->real_priority = new_prio;
        /* TODO: confirm adjust_priority() is needed here as well as below. */
        CRITICAL_INHERIT(act, thp);
        adjust_priority(act, new_prio, new_dpp, 1);
    }

    return EOK;
}

/* Read a (partially received) message from a client into the server's iov
 * (msg_readv kcall).
 *
 * Transfers up to the requested length from the reply-blocked client thread
 * named by kap->rcvid into the server's buffers, claiming the client via
 * MSG_DELIVERY so a concurrent reply/error cannot overwrite the source
 * mid-read. Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (rcvid, destination iov rmsg/rparts, offset)
 * Return value: ENOERROR (transferred length or error reported via
 * KSTATUS/kcall_err).
 */
int kdecl ksys_msg_readv(thread_t *act, struct kcall_args_msg_readv *kap)
{
    connect_t *cop;
    thread_t *thp;
    int xferstat;
    unsigned slen;
    int do_force_ready = 0;
    unsigned thp_status = 0;

    /* channel_slock synchronizes the state recheck and the MSG_DELIVERY claim
     * with ksys_msg_replyv / ksys_msg_error / force_ready. The short-msg path
     * reads thp->args.msbuff.buff under the same MSG_DELIVERY claim discipline
     * as the long-msg path, so a concurrent replyv cannot overwrite the source
     * buffer mid-read.
     */

    if ((cop = lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) ==
        NULL) {
        return ENOERROR;
    }

    /* Make sure thread is replied blocked on a channel owned by this process.
     */
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        return ESRCH;
    }

    /* Verify that the message has been fully received, and that the receiving
     * thread has completed any specialret() processing that needs to be done.
     */
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        return ESRCH;
    }

    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;

        /* NULL: client's blocked_on was cleared by a concurrent teardown
         * (force_ready on a client whose process is exiting) -> ESRCH, matching
         * the locked recheck and the guard in fastmsg.c. Without it the deref
         * below load-faults near NULL.
         */
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
            return ESRCH;
        }
    }

    act->args.ms.rmsg = kap->rmsg;
    act->args.ms.rparts = kap->rparts;

    /* Validate iov_t boundary */
    if (kap->rparts < 0) {
        /* Single part */
        uintptr_t base, last;
        size_t len;

        base = (uintptr_t) kap->rmsg;
        len = PARTS_TO_LEN(kap->rparts);
        last = base + len - 1;
        if ((base > last) ||
            !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) {
            return EFAULT;
        }
    } else if (kap->rparts == 1) {
        /* One iov_t */
        uintptr_t base, last;
        size_t len;

        base = (uintptr_t) GETIOVBASE(kap->rmsg);
        len = GETIOVLEN(kap->rmsg);
        last = base + len - 1;
        if (((base > last) ||
             !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) &&
            (len != 0)) {
            return EFAULT;
        }
    } else {
        /* Multi-iov_t. Walk IOVs for boundary check. */
        uintptr_t base, last;
        size_t len;
        int rparts = kap->rparts;
        iov_t *iov = kap->rmsg;

        while (rparts) {
            base = (uintptr_t) GETIOVBASE(iov);
            len = GETIOVLEN(iov);
            last = base + len - 1;
            if (((base > last) ||
                 !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) &&
                (len != 0)) {
                return EFAULT;
            }
            iov++;
            --rparts;
        }
    }

    /* State-recheck + MSG_DELIVERY claim under channel_slock. Synchronizes with
     * ksys_msg_replyv (which takes the same slock for its own claim). If state
     * has flipped or another path already holds the claim, drop and bail with
     * ESRCH.
     */
    kspinlock_lock(&cop->channel->lock);
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        /* Another path holds the claim — replyv, error, or another concurrent
         * readv/writev/readiov. Bail; user can retry.
         */
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->blocked_on != cop) {
        /* The sender re-blocked on a different cop after the lock acquire. */
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    kspinlock_unlock(&cop->channel->lock);

    /* From here on thp is claimed: replyv / error / force_ready will defer (the
     * MSG_DELIVERY check inside their own channel_slock windows skips claimed
     * threads). The xfer can proceed without holding channel_slock.
     */

    if (thp->flags & RTMUX_FLG_THR_BUFF_MSG) {
        unsigned dlen;

        slen = thp->args.msbuff.msglen;
        if (kap->offset < slen) {
            slen -= kap->offset;
            xferstat = xfer_cpy_diov(act, act->args.ms.rmsg,
                                     thp->args.msbuff.buff + kap->offset,
                                     act->args.ms.rparts, slen);
            dlen =
                (unsigned) xferlen(act, act->args.ms.rmsg, act->args.ms.rparts);
            if (dlen < slen)
                slen = dlen;
        } else {
            /* No data to transfer */
            xferstat = 0;
            slen = 0;
        }
    } else {
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)

        /* Per-act in-flight markers. These are single-writer-by-CPU on "act"
         * and don't need slock; the cross-CPU claim on thp is held via
         * MSG_DELIVERY above.
         */
        *(volatile void **) &(act->blocked_on) = thp;
        ITF_SET(act, RTMUX_ITF_MSG_DELIVERY);
#endif
        xferstat = xfermsg(act, thp, 0, kap->offset);
        slen = thp->args.ms.msglen;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        ITF_CLR(act, RTMUX_ITF_MSG_DELIVERY);
        act->blocked_on = 0;
#endif
    }

    /* Claim release + MSG_FORCE_RDY drain under channel_slock. If another path
     * queued a force_ready against thp during this xfer (it sees MSG_DELIVERY
     * set and defers via the MSG_FORCE_RDY bit), honor it now — outside
     * channel_slock since force_ready takes sched_slock and SCHED < CHANNEL
     * would be a rank inversion.
     */
    kspinlock_lock(&cop->channel->lock);
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
    if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
        ITF_CLR(thp, RTMUX_ITF_MSG_FORCE_RDY);
        thp_status = MSG_FORCE_ERROR(thp);
        do_force_ready = 1;
    }
    kspinlock_unlock(&cop->channel->lock);
    if (do_force_ready)
        force_ready(thp, thp_status);

    if (xferstat) {
        kcall_err(act, (xferstat & XFER_SRC_FAULT) ? EFAULT : EFAULT);
    } else {
        SETKSTATUS(act, slen);
    }

    act->restart = NULL;
    return ENOERROR;
}



/* Write into a client's reply buffer from the server's iov (msg_writev kcall).
 *
 * Transfers from the server's send iov into the reply-blocked client thread
 * named by kap->rcvid, claiming the client via MSG_DELIVERY so a concurrent
 * reply cannot overwrite the client's reply iov mid-write. Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (rcvid, source iov smsg/sparts, offset)
 * Return value: ENOERROR (transferred length or error reported via
 * KSTATUS/kcall_err).
 */
int kdecl ksys_msg_writev(thread_t *act, struct kcall_args_msg_writev *kap)
{
    connect_t *cop;
    thread_t *thp;
    int xferstat;
    int do_force_ready = 0;
    unsigned thp_status = 0;

    /* Same canonical IPC-kcall shape as ksys_msg_readv: channel_slock for
     * state-recheck + MSG_DELIVERY claim + claim-release, xfer outside the
     * slock, force_ready outside (sched_slock-internal, SCHED < CHANNEL rank).
     */

    if ((cop = lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) ==
        NULL) {
        return ENOERROR;
    }

    /* Make sure thread is replied blocked on a channel owned by this process.
     */
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        return ESRCH;
    }

    /* Verify that the message has been fully received, and that the receiving
     * thread has completed any specialret() processing that needs to be done.
     */
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        return ESRCH;
    }

    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;

        /* NULL: client's blocked_on was cleared by a concurrent teardown
         * (force_ready on a client whose process is exiting) -> ESRCH, matching
         * the locked recheck and the guard in fastmsg.c. Without it the deref
         * below load-faults near NULL.
         */
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
            return ESRCH;
        }
    }
    /* Validate iov_t boundary */
    if (kap->sparts < 0) {
        /* Single part */
        uintptr_t base, last;
        size_t len;

        base = (uintptr_t) kap->smsg;
        len = PARTS_TO_LEN(kap->sparts);
        last = base + len - 1;
        if ((base > last) ||
            !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) {
            return EFAULT;
        }
    } else if (kap->sparts == 1) {
        /* One iov_t */
        uintptr_t base, last;
        size_t len;

        base = (uintptr_t) GETIOVBASE(kap->smsg);
        len = GETIOVLEN(kap->smsg);
        last = base + len - 1;
        if (((base > last) ||
             !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) &&
            (len != 0)) {
            return EFAULT;
        }
    } else {
        /* Multi-iov_t. Walk IOVs for boundary check. */
        uintptr_t base, last;
        size_t len;
        int rparts = kap->sparts;
        iov_t *iov = kap->smsg;

        while (rparts) {
            base = (uintptr_t) GETIOVBASE(iov);
            len = GETIOVLEN(iov);
            last = base + len - 1;
            if (((base > last) ||
                 !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) &&
                (len != 0)) {
                return EFAULT;
            }
            iov++;
            --rparts;
        }
    }

    /* State-recheck + MSG_DELIVERY claim under channel_slock. Synchronizes with
     * ksys_msg_replyv / ksys_msg_error. If state has flipped or another path
     * holds the claim, drop and bail with ESRCH. thp->args.ri.{rmsg,rparts}
     * (the xfer destination below) stay stable while thp is STATE_REPLY +
     * claimed; replyv would overwrite them otherwise.
     */
    kspinlock_lock(&cop->channel->lock);
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->blocked_on != cop) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    kspinlock_unlock(&cop->channel->lock);

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    /* Per-act in-flight markers (single-writer-by-CPU). */
    *(volatile void **) &(act->blocked_on) = thp;
    ITF_SET(act, RTMUX_ITF_MSG_DELIVERY);
#endif
    act->args.ms.smsg = kap->smsg;
    act->args.ms.sparts = kap->sparts;
    xferstat = xfermsg(thp, act, kap->offset, 0);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    ITF_CLR(act, RTMUX_ITF_MSG_DELIVERY);
    act->blocked_on = 0;
#endif

    /* Claim release + MSG_FORCE_RDY drain under channel_slock. */
    kspinlock_lock(&cop->channel->lock);
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
    if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
        ITF_CLR(thp, RTMUX_ITF_MSG_FORCE_RDY);
        thp_status = MSG_FORCE_ERROR(thp);
        do_force_ready = 1;
    }
    kspinlock_unlock(&cop->channel->lock);
    if (do_force_ready)
        force_ready(thp, thp_status);

    if (xferstat) {
        kcall_err(act, (xferstat & XFER_SRC_FAULT) ? EFAULT : EFAULT);
    } else {
        SETKSTATUS(act, act->args.ms.msglen);
    }

    act->restart = 0;
    return ENOERROR;
}



/* Read a message into the server's iov, mapping the client's iov directly
 * (msg_readiov kcall).
 *
 * Like msg_readv, but the source is described as the client's send (or reply)
 * iov and is mapped part-by-part via viov_to_piov. The client is claimed via
 * MSG_DELIVERY for the duration of the walk. Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (rcvid, destination iov, offset, REPLY/SEND flag)
 * Return value: ENOERROR (transferred length or error reported via KSTATUS).
 */
int kdecl ksys_msg_readiov(thread_t *act, struct kcall_args_msg_readiov *kap)
{
    connect_t *cop;
    thread_t *thp;
    int n;
    int do_force_ready = 0;
    unsigned thp_status = 0;
    iov_t *src_msg;
    int src_parts;

    /* Same canonical IPC-kcall shape as ksys_msg_readv / ksys_msg_writev:
     * channel_slock recheck + MSG_DELIVERY claim, viov_to_piov walk outside the
     * slock with the claim held to gate replyv, claim release + MSG_FORCE_RDY
     * drain, force_ready outside (sched_slock-internal, SCHED < CHANNEL rank).
     */

    if ((cop = lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) ==
        NULL) {
        return ENOERROR;
    }

    /* Make sure thread is replied blocked on a channel owned by this process.
     */
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        return ESRCH;
    }

    /* Verify that the message has been fully received, and that the receiving
     * thread has completed any specialret() processing that needs to be done.
     */
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        return ESRCH;
    }

    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;

        /* NULL: client's blocked_on was cleared by a concurrent teardown
         * (force_ready on a client whose process is exiting) -> ESRCH, matching
         * the locked recheck and the guard in fastmsg.c. Without it the deref
         * below load-faults near NULL.
         */
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
            return ESRCH;
        }
    }

    /* State-recheck + MSG_DELIVERY claim under channel_slock. The claim keeps
     * replyv off thp during the thp->args.ms.* walk via viov_to_piov.
     */
    kspinlock_lock(&cop->channel->lock);
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->blocked_on != cop) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }

    /* Snapshot the iov-source fields under the slock — once it is released
     * MSG_DELIVERY alone keeps them stable, but the cached values suffice.
     */
    if (kap->flags & RTMUX_READIOV_REPLY) {
        src_msg = thp->args.ms.rmsg;
        src_parts = thp->args.ms.rparts;
    } else {
        src_msg = thp->args.ms.smsg;
        src_parts = thp->args.ms.sparts;
    }
    ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    kspinlock_unlock(&cop->channel->lock);

    n = viov_to_piov(thp->process, kap->iov, kap->parts, src_msg, src_parts,
                     (int) kap->offset);

    /* Claim release + MSG_FORCE_RDY drain under channel_slock. */
    kspinlock_lock(&cop->channel->lock);
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
    if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
        ITF_CLR(thp, RTMUX_ITF_MSG_FORCE_RDY);
        thp_status = MSG_FORCE_ERROR(thp);
        do_force_ready = 1;
    }
    kspinlock_unlock(&cop->channel->lock);
    if (do_force_ready)
        force_ready(thp, thp_status);

    if (n < 0) {
        return -n;
    }

    SETKSTATUS(act, n);
    return ENOERROR;
}



/* Fill a _msg_info for a received message (msg_info kcall).
 *
 * Snapshots the receive info for the reply-blocked client thread named by
 * kap->rcvid into the caller's _msg_info buffer. Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (rcvid, info buffer)
 * Return value: EOK on success; ESRCH if the rcvid no longer names a
 *               reply-blocked client.
 */
int kdecl ksys_msg_info(thread_t *act, struct kcall_args_msg_info *kap)
{
    thread_t *thp;
    connect_t *cop;
    struct _msg_info info;
    struct user_slice info_slice;

    /* Pure reader. WR_VERIFY_PTR / WR_PROBE_INT happen before the
     * non-preemptible section so caller-page faulting can use the regular fault
     * path. channel_slock then protects the get_rcvinfo snapshot against a
     * concurrent ksys_msg_replyv / ksys_msg_error mutating thp's args/state
     * mid-read. No MSG_DELIVERY claim is needed — this is not a long xfer that
     * would block the writer; the slock window covers the entire snapshot.
     */
    if (user_slice_init(act, kap->info, sizeof(*kap->info), sizeof(*kap->info),
                        USER_SLICE_WR, &info_slice))
        return EFAULT;

    if (!(cop = lookup_rcvid(NULL, kap->rcvid, &thp))) {
        return ENOERROR;
    }

    /* Make sure thread is replied blocked on a channel owned by this process.
     */
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        return ESRCH;
    }

    /* Verify that the message has been fully received, and that the receiving
     * thread has completed any specialret() processing that needs to be done.
     */
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        return ESRCH;
    }

    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;

        /* NULL: client's blocked_on was cleared by a concurrent teardown
         * (force_ready on a client whose process is exiting) -> ESRCH, matching
         * the locked recheck and the guard in fastmsg.c. Without it the deref
         * below load-faults near NULL.
         */
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
            return ESRCH;
        }
    }

    /* channel_slock recheck + full get_rcvinfo snapshot under the lock.
     * Releases on every error path.
     */
    kspinlock_lock(&cop->channel->lock);
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }
    if (thp->blocked_on != cop) {
        kspinlock_unlock(&cop->channel->lock);
        return ESRCH;
    }

    /* Zeroed before the fill because the copy-out below moves the whole struct,
     * padding included, not field by field the way the old direct write did.
     * get_rcvinfo happens to set all fourteen members and the layout happens to
     * have no holes today, so nothing leaks; a future field or a padding hole
     * would leak kernel stack silently, and this costs 56 bytes of zeroing on a
     * diagnostic path.
     */
    memset(&info, 0, sizeof info);
    get_rcvinfo(thp, -1, cop, &info);
    kspinlock_unlock(&cop->channel->lock);

    /* Snapshot under the lock, copy out after it. get_rcvinfo used to write
     * straight into the caller's buffer while cop->channel->lock was held: a
     * sibling thread unmapping the page in that window faulted S-mode with the
     * lock held and no handler armed, so the channel lock was never released
     * and every later MsgSend/Receive on it spun forever. The entry probe
     * rejects the deliberate case but cannot promise the mapping survives to
     * the store, which is why the copy is fault-safe as well as unlocked.
     */
    if (user_slice_copyout(&info_slice, &info) != 0)
        return EFAULT;

    return EOK;
}


/* Deliver a sigevent on behalf of a client (msg_deliver_event kcall).
 *
 * Executes a copy of the caller-supplied sigevent against the destination
 * thread named by kap->rcvid. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (rcvid, event)
 * Return value: EOK on success; the sigevent_exe status on failure.
 */
int kdecl ksys_msg_deliver_event(thread_t *act,
                                 struct kcall_args_msg_deliver_event *kap)
{
    thread_t *thp;
    connect_t *cop;
    int status;
    unsigned cpu;
    struct sigevent event;

    if (user_slice_get(act, &event, kap->event, sizeof(event)))
        return EFAULT;

    /* The body's only kernel-shared intent is "which heap should a notification
     * allocation tap"; expressed via the per-CPU soul_alloc_override slot below
     * — no shared state mutated.
     *
     * sigevent_exe's internal locks (channel_slock, sched_slock, alloc_slock)
     * are self-acquired on its sub-paths.
     */

    /* Make sure connection is valid and extract destination thread. */
    if ((cop = lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) ==
        NULL) {
        return ENOERROR;
    }

    /* Tell object_alloc (via the per-CPU override) that any notification
     * allocated for this user-requested event should NOT tap the critical
     * reserve — the critical heap is for hwintrs and fault-delivery, not for
     * user-supplied SIGEV_NOTIFICATION arming.
     */
    cpu = get_cpunum();
    pcpu_array[cpu].pc_soul_alloc_override = SOUL_OVERRIDE_NONCRITICAL;
    status = sigevent_exe(&event, thp, 1, 0);
    pcpu_array[cpu].pc_soul_alloc_override = SOUL_OVERRIDE_NONE;


    if (status != 0) {
        return status;
    }

    return EOK;
}


/* Validate a sigevent without delivering it (msg_verify_event kcall).
 *
 * Runs sigevent_exe in verify-only mode against the calling thread, or the
 * destination thread named by kap->rcvid when one is given. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (optional rcvid, event)
 * Return value: EOK if the event is valid; the sigevent_exe status otherwise.
 */
int kdecl ksys_msg_verify_event(thread_t *act,
                                struct kcall_args_msg_verify_event *kap)
{
    thread_t *thp;
    int status;
    struct sigevent event;

    /* Copy after the checks, not in the initializer: the declaration used to
     * dereference the caller's pointer one line ahead of its own range check,
     * so a null or unmapped event was an unprivileged kernel fault with the
     * check still pending.
     */
    if (user_slice_get(act, &event, kap->event, sizeof(event)))
        return EFAULT;

    /* Pure verifier. sigevent_exe(deliver=0) is read-only — no notification
     * allocation, no force_ready, no kernel state mutation. Nothing to slock
     * against.
     */

    /* Use calling thread, or extract destination thread if rcvid given. */
    thp = act;
    if (kap->rcvid &&
        !lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) {
        return ENOERROR;
    }

    status = sigevent_exe(&event, thp, 0, 0);

    if (status != 0) {
        return status;
    }
    return EOK;
}


/* Bounds on the message a key covers: the _IO_DUP/_IO_OPENFD headers are far
 * smaller; the cap keeps the unprivileged VERIFY path's hash work and user
 * reads small and fixed.
 */
#define KEYDATA_MAX_PARTS 4
#define KEYDATA_MAX_BYTES 256

/* KEYDATA_MAX_FAILS (<sys/rtmux-core.h>): failed VERIFYs a minted secret
 * survives. VERIFY is open to any caller, so this bounds online guessing of the
 * 32-bit wire key to a handful of tries per minted secret instead of an
 * unthrottled 2^32 search.
 */

/* Generate or verify a message data key (msg_key_data kcall).
 *
 * CALCULATE requires RTMUX_PRIV_CAP_IPC. It draws a fresh random 64-bit secret
 * for the destination thread (the caller's seed argument is ignored), stores
 * it, and returns a SipHash tag of the message under it. VERIFY needs no cap:
 * it only compares against the secret of a client thread on one of the caller's
 * own connections, so any server may check a key a privileged minter issued. A
 * thread with no secret (0) never verifies, and a secret is retired after
 * KEYDATA_MAX_FAILS wrong keys, counted atomically. A server connected to the
 * thread can exhaust that budget; this costs only the thread's key
 * (availability, never authenticity) and the minter reissues it. The count is
 * deliberately not per verifier: any bounded per-verifier table can be evicted
 * by a caller that cycles through enough verifier pids, which would also reset
 * its guess budget, so the single counter is the conservative bound.
 *
 * The message bytes are copied into a kernel buffer with a fault-safe copy and
 * only the copy is hashed, so a concurrent unmap cannot fault the kernel.
 * Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (rcvid, op, key, message parts, newkey out)
 * Return value: EOK on success; an errno on failure.
 */
int kdecl ksys_msg_keydata(thread_t *act, struct kcall_args_msg_keydata *kap)
{
    thread_t *thp;
    connect_t *cop;
    iov_t parts[KEYDATA_MAX_PARTS];
    unsigned char buf[KEYDATA_MAX_BYTES];
    size_t total = 0;
    int32_t i;
    uint32_t newkey;
    struct user_slice key_slice;

    /* Reissuing a message key is a net-IPC authority, gated by
     * RTMUX_PRIV_CAP_IPC rather than the root credential; a non-holder is
     * refused with EPERM. A loading child holds every cap transiently, so
     * exclude that window. VERIFY only reads and needs no cap.
     */
    if (kap->op != RTMUX_KEYDATA_VERIFY &&
        ((act->process->flags & RTMUX_FLG_PROC_LOADING) ||
         !(act->process->priv_caps & RTMUX_PRIV_CAP_IPC)))
        return EPERM;

    keydata_stir();

    if (user_slice_init(act, kap->newkey, sizeof(*kap->newkey),
                        sizeof(*kap->newkey), USER_SLICE_WR, &key_slice))
        return EFAULT;

    if (kap->parts < 0 || kap->parts > KEYDATA_MAX_PARTS)
        return EINVAL;
    if (kap->parts > 0 && user_slice_get(act, parts, kap->msg,
                                         sizeof(iov_t) * (size_t) kap->parts))
        return EFAULT;
    for (i = 0; i < kap->parts; i++) {
        size_t len = GETIOVLEN(&parts[i]);

        if (len > KEYDATA_MAX_BYTES - total)
            return EMSGSIZE;
        if (len == 0)
            continue;
        if (user_slice_get(act, buf + total, GETIOVBASE(&parts[i]), len))
            return EFAULT;
        total += len;
    }

    /* Make sure connection is valid and extract destination thread. */
    if ((cop = vector_lookup(&act->process->chancons, MCINDEX(kap->rcvid))) ==
            NULL ||
        (cop->type != TYPE_CONNECTION) ||
        (thp = vector_lookup(&cop->process->threads, MTINDEX(kap->rcvid))) ==
            NULL) {
        return ESRCH;
    }

    if (kap->op != RTMUX_KEYDATA_VERIFY) { /* Calculate key. */
        uint64_t secret = keydata_draw((uintptr_t) act ^ (uintptr_t) thp);

        /* Reset the budget before publishing the new secret so a concurrent
         * VERIFY of the old one cannot retire the fresh secret.
         */
        __atomic_store_n(&thp->key_fail, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&thp->key, secret, __ATOMIC_RELEASE);
        newkey = keygen(buf, total, secret);
    } else { /* Verify key */
        uint64_t secret = __atomic_load_n(&thp->key, __ATOMIC_ACQUIRE);

        if (secret != 0 && keygen(buf, total, secret) == kap->key) {
            newkey = 0;
        } else {
            newkey = ~0u;

            /* Too many wrong guesses: retire the secret, but only the one this
             * VERIFY read, so a racing re-mint is never zeroed.
             */
            if (secret != 0 &&
                __atomic_add_fetch(&thp->key_fail, 1, __ATOMIC_RELAXED) >=
                    KEYDATA_MAX_FAILS) {
                uint64_t expect = secret;

                (void) __atomic_compare_exchange_n(&thp->key, &expect, 0, 0,
                                                   __ATOMIC_ACQ_REL,
                                                   __ATOMIC_RELAXED);
            }
        }
    }

    return user_slice_copyout(&key_slice, &newkey) ? EFAULT : EOK;
}

/* ksys_msg_readwritev is removed: no in-tree caller exercised it. Slot 18
 * (__KSYS_MSG_READWRITEV) is bound to ksys_bad in the dispatch table. The enum
 * slot number stays for ABI positional integrity (every later slot's number
 * depends on it).
 */
