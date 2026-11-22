/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <core/globals.h>
#include "reply-client.h"
#include <user-slice.h>
/* #define   MSG_XFER_CHUNK_SIZE     1024 */


static int msg_current_take(thread_t *act, struct kcall_args_msg_current *kap);

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
 *               reply-blocked client. Either way the loan of the prior client
 *               has ended.
 */
int kdecl ksys_msg_current(thread_t *act, struct kcall_args_msg_current *kap)
{
    bool ended;
    int r;

    /* Disown act's prior client, if any, and end the loan it lent, whatever
     * this call then does: a call that fails must not leave act running at the
     * priority of a client it no longer serves. The prior client briefly
     * observes server=0 with act->client cleared; eventually consistent for its
     * own ksys_msg_current.
     */
    ended = thread_disown_client(act);
    if (ended)
        CRITICAL_CLEAR(act);

    r = kap->rcvid == 0 ? EOK : msg_current_take(act, kap);

    /* A new client's loan, once taken, replaced the ended one and was applied
     * by msg_current_take; with none, or on an error return, apply the end.
     * Applying can switch act out, and the trap path writes a returned status
     * only while act still runs, so publish the status first.
     */
    if (ended && (kap->rcvid == 0 || r != EOK)) {
        if (r != ENOERROR) {
            if (r == EOK)
                SETKSTATUS(act, EOK);
            else
                kcall_err(act, r);
            r = ENOERROR;
        }
        thread_prio_update(act);
    }
    return r;
}

/* Make the client named by kap->rcvid act's current one and lend its priority.
 *
 * Returns EOK on success, ESRCH if the rcvid no longer names a reply-blocked
 * client, or ENOERROR when lookup_rcvid already set the error.
 */
static int msg_current_take(thread_t *act, struct kcall_args_msg_current *kap)
{
    connect_t *cop;
    thread_t *thp;
    int do_pi = 0;
    kdispatch_t *new_dpp = NULL;

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

    cop = reply_client_connection(thp, cop);
    if (!cop) {
        return ESRCH;
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

    rcu_read_lock(&tthread_rcu);
    thread_t *previous = __atomic_load_n(&thp->args.ms.server, __ATOMIC_ACQUIRE);
    bool settle_previous = previous && previous != act &&
                           thread_client_departed(previous, thp);

    thp->args.ms.server = act;
    act->client = thp;

    /* Snapshot the PI inputs while holding cop->lock; cop->lock must be dropped
     * before adjust_priority() because adjust_priority acquires sched_slock
     * (SCHED), and SCHED < CONNECT — the rank rule forbids holding cop->lock
     * while taking sched_slock.
     */
    if ((cop->channel->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
        do_pi = 1;
        new_dpp = thp->dpp;
    }

    kspinlock_unlock(&cop->lock);

    if (settle_previous)
        thread_prio_defer_settle(previous);
    rcu_read_unlock(&tthread_rcu);

    /* The loan update can displace act before the dispatcher writes status. */
    SETKSTATUS(act, EOK);
    /* PI propagation — outside cop->lock to honor SCHED-vs-CONNECT rank. */
    if (do_pi) {
        /* act now serves thp, which lends its priority until act replies. Read
         * thp's priority after act->client = thp is published, not snapshotted
         * under cop->lock: a REPLY propagation that raced the snapshot fails
         * its swap against the generation this lend starts, so a stale value
         * here would stay.
         */
        CRITICAL_INHERIT(act, thp);
        reply_client_priority(act, thp, new_dpp);
    } else {
        /* A fixed-priority channel lends nothing. Drop the loan an earlier
         * client left, or a REPLY propagation from thp would relend it.
         */
        reply_client_priority(act, NULL, NULL);
    }

    return EOK;
}

/* Forget what a restarted msg_readv or msg_writev kept for its next entry: the
 * vector it checked, the cursor of its copy and its restart link.
 */
static void msg_resume_drop(thread_t *act)
{
    act->iovchk_iov = NULL;
    act->xcur.armed = false;
    act->restart = NULL;
}

/* Check the caller's own iov array against its boundary, at most
 * XFER_IOV_CHECK_STEP entries per kernel entry. parts is at least 1.
 *
 * msg_readv and msg_writev check their iov first of all, before they look up or
 * claim the client, so there is nothing to undo when the call restarts and
 * every return path of the call has consumed or dropped the saved prefix. The
 * checked prefix is kept on the thread (iovchk_*), keyed by the array and its
 * length, so a restart resumes where this entry stopped instead of rereading
 * from entry 0. Each entry that has more to check ends with KCALL_RESTART,
 * which returns through kernel_exit and lets a higher-priority thread run, so
 * no kernel entry spends more than one step on the check. The trap handler
 * drops the saved prefix at the next kernel call that is not msg_readv or
 * msg_writev, so this thread cannot edit the entries it has already passed
 * between the restart and the resume. Another thread sharing the array can, so
 * the transfer paths still check the entries they use. Entries are read through
 * xfer_memchk, which recovers from a fault.
 *
 * Returns 0 (all entries are inside the boundary), EFAULT (one is not, a page
 * of the array is unmapped, the array itself is not in the caller's space, or
 * it is longer than XFER_IOV_LIMIT), or XFER_CALL_RESTART (the call was
 * restarted: return ENOERROR).
 */
static int xfer_iov_check(thread_t *act, const iov_t *iov, int parts)
{
    uint32_t done = 0, n;
    int bad;
    const iov_t *saved = act->iovchk_iov;
    uintptr_t base = (uintptr_t) iov, last;

    act->iovchk_iov = NULL;
    if (parts > XFER_IOV_LIMIT)
        return EFAULT;

    /* The array itself must lie in the caller's space: a fault-recovering read
     * still succeeds on a mapped kernel address.
     */
    last = base + (size_t) parts * sizeof(iov_t) - 1;
    if (!base || base > last ||
        !WITHIN_BOUNDARY(base, last, act->process->boundary_addr))
        return EFAULT;
    if (saved == iov && act->iovchk_parts == parts)
        done = act->iovchk_done;

    /* Checked already by an earlier entry of this call, which restarted in the
     * copy: the copy reads and checks every entry it uses, so do not walk the
     * vector again on each restart.
     */
    if (done == (uint32_t) parts) {
        act->iovchk_iov = iov;
        return 0;
    }

    n = (uint32_t) parts - done;
    if (n > XFER_IOV_CHECK_STEP)
        n = XFER_IOV_CHECK_STEP;
    SET_XFER_HANDLER(&xfer_src_handlers);
    bad = xfer_memchk(act->process->boundary_addr, iov + done, n);
    SET_XFER_HANDLER(0);
    if (bad)
        return EFAULT;
    done += n;
    act->iovchk_iov = iov;
    act->iovchk_parts = parts;
    act->iovchk_done = done;
    if (done == (uint32_t) parts)
        return 0;
    KCALL_RESTART(act);
    return XFER_CALL_RESTART;
}

/* Check the caller's own iov (the destination of msg_readv, the source of
 * msg_writev) against the process boundary, before the client is claimed.
 *
 * It runs first, ahead of lookup_rcvid(), so that a saved xfer_iov_check()
 * prefix is always consumed or dropped by the call it belongs to, and so that
 * every later path, the remote one included, sees a vector that is in range and
 * no longer than XFER_IOV_LIMIT. A negative count is one flat buffer, which has
 * no entries to read; an entry is read only by xfer_iov_check(), which recovers
 * from a fault and does at most one step of the work per kernel entry.
 *
 * Returns 0, an errno, or XFER_CALL_RESTART when the call was restarted.
 */
static int msg_iov_check(thread_t *act, const iov_t *iov, int parts)
{
    if (parts < 0) {
        uintptr_t base = (uintptr_t) iov, last;

        act->iovchk_iov = NULL;
        last = base + PARTS_TO_LEN(parts) - 1;
        if (base > last ||
            !WITHIN_BOUNDARY(base, last, act->process->boundary_addr))
            return EFAULT;
        return 0;
    }
    if (parts == 0) {
        act->iovchk_iov = NULL;
        return 0;
    }
    return xfer_iov_check(act, iov, parts);
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
static int msg_readv_body(thread_t *act, struct kcall_args_msg_readv *kap)
{
    connect_t *cop;
    thread_t *thp;
    int xferstat, status;
    unsigned slen;
    int do_force_ready = 0;
    unsigned thp_status = 0;

    /* channel_slock synchronizes the state recheck and the MSG_DELIVERY claim
     * with ksys_msg_replyv / ksys_msg_error / force_ready. The short-msg path
     * reads thp->args.msbuff.buff under the same MSG_DELIVERY claim discipline
     * as the long-msg path, so a concurrent replyv cannot overwrite the source
     * buffer mid-read.
     */

    status = msg_iov_check(act, kap->rmsg, kap->rparts);
    if (status)
        return status;

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

    cop = reply_client_connection(thp, cop);
    if (!cop) {
        return ESRCH;
    }

    act->args.ms.rmsg = kap->rmsg;
    act->args.ms.rparts = kap->rparts;

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
        if (kap->offset < slen && act->args.ms.rparts > 1) {
            /* A destination of many entries is walked a step at a time, so a
             * vector of empty ones does not hold the kernel to its end; the
             * cursor is the entry reached and the bytes placed, for the same
             * client, vector and offset.
             */
            unsigned idx = 0;
            size_t copied = 0;

            slen -= kap->offset;
            if (act->xcur.armed && act->xcur.sthp == thp &&
                act->xcur.rmsg == act->args.ms.rmsg &&
                act->xcur.rparts == (int32_t) act->args.ms.rparts &&
                act->xcur.soff0 == kap->offset && act->xcur.msglen == slen) {
                idx = act->xcur.didx;
                copied = act->xcur.soff;
            }
            xferstat = xfer_cpy_diov_step(
                act, act->args.ms.rmsg, act->args.ms.rparts,
                thp->args.msbuff.buff + kap->offset, slen, &idx, &copied);
            if (xferstat == XFER_YIELD) {
                act->xcur.armed = true;
                act->xcur.sthp = thp;
                act->xcur.rmsg = act->args.ms.rmsg;
                act->xcur.rparts = (int32_t) act->args.ms.rparts;
                act->xcur.soff0 = kap->offset;
                act->xcur.msglen = (uint32_t) slen;
                act->xcur.didx = idx;
                act->xcur.soff = copied;
                KCALL_RESTART(act);
                xferstat = XFER_RESTART;
            } else {
                act->xcur.armed = false;
                slen = (unsigned) copied;
            }
        } else if (kap->offset < slen) {
            slen -= kap->offset;
            xferstat = xfer_cpy_diov(act, act->args.ms.rmsg,
                                     thp->args.msbuff.buff + kap->offset,
                                     act->args.ms.rparts, slen);
            dlen = (unsigned) xferlen_upto(act, act->args.ms.rmsg,
                                           act->args.ms.rparts, slen);
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
        xferstat = xfermsg_bounded(act, thp, 0, kap->offset);
        slen = thp->args.ms.msglen;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        ITF_CLR(act, RTMUX_ITF_MSG_DELIVERY);
        act->blocked_on = 0;
        IPC_BLK(act);
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
        thp_status = (unsigned) KSTATUS(thp);
        do_force_ready = 1;
    }
    kspinlock_unlock(&cop->channel->lock);
    if (do_force_ready)
        force_ready(thp, thp_status);

    /* The copy used up its step and arranged for the call to run again; the
     * client is given up above, so another thread can reply to it, and is
     * claimed again by the restarted call.
     */
    if (xferstat == XFER_RESTART)
        return XFER_CALL_RESTART;

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
static int msg_writev_body(thread_t *act, struct kcall_args_msg_writev *kap)
{
    connect_t *cop;
    thread_t *thp;
    int xferstat, status;
    int do_force_ready = 0;
    unsigned thp_status = 0;

    /* Same canonical IPC-kcall shape as ksys_msg_readv: channel_slock for
     * state-recheck + MSG_DELIVERY claim + claim-release, xfer outside the
     * slock, force_ready outside (sched_slock-internal, SCHED < CHANNEL rank).
     */

    status = msg_iov_check(act, kap->smsg, kap->sparts);
    if (status)
        return status;

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

    cop = reply_client_connection(thp, cop);
    if (!cop) {
        return ESRCH;
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
    xferstat = xfermsg_bounded(thp, act, kap->offset, 0);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    ITF_CLR(act, RTMUX_ITF_MSG_DELIVERY);
    act->blocked_on = 0;
    IPC_BLK(act);
#endif

    /* Claim release + MSG_FORCE_RDY drain under channel_slock. */
    kspinlock_lock(&cop->channel->lock);
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
    if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
        ITF_CLR(thp, RTMUX_ITF_MSG_FORCE_RDY);
        thp_status = (unsigned) KSTATUS(thp);
        do_force_ready = 1;
    }
    kspinlock_unlock(&cop->channel->lock);
    if (do_force_ready)
        force_ready(thp, thp_status);

    /* The copy used up its step and arranged for the call to run again; the
     * client is given up above, so another thread can reply to it, and is
     * claimed again by the restarted call.
     */
    if (xferstat == XFER_RESTART)
        return XFER_CALL_RESTART;

    if (xferstat) {
        kcall_err(act, (xferstat & XFER_SRC_FAULT) ? EFAULT : EFAULT);
    } else {
        SETKSTATUS(act, act->args.ms.msglen);
    }

    act->restart = 0;
    return ENOERROR;
}



/* msg_readv and msg_writev: the iov check first, then the transfer. A call that
 * returns here is over, so the check's saved state (it holds the vector as
 * checked, for a restart of the copy) is dropped on every path but a restart,
 * whether the check's or the copy's, which both return XFER_CALL_RESTART.
 * ENOERROR is a finished call (its status already stored), not a restart.
 */
int kdecl ksys_msg_readv(thread_t *act, struct kcall_args_msg_readv *kap)
{
    int rc = msg_readv_body(act, kap);

    if (rc == XFER_CALL_RESTART)
        return ENOERROR;
    msg_resume_drop(act);
    return rc;
}

int kdecl ksys_msg_writev(thread_t *act, struct kcall_args_msg_writev *kap)
{
    int rc = msg_writev_body(act, kap);

    if (rc == XFER_CALL_RESTART)
        return ENOERROR;
    msg_resume_drop(act);
    return rc;
}

/* Read a message into the server's iov, mapping the client's iov directly
 * (msg_readiov kcall).
 *
 * Like msg_readv, but the source is described as the client's send (or reply)
 * iov and is mapped part-by-part via viov_to_piov. The client is claimed via
 * MSG_DELIVERY for the duration of the walk. The walk is capped per call (see
 * VIOV_STEP_MAX), entries skipped by the offset included, so the parts may
 * cover only a prefix of the message; the caller continues at the offset they
 * reach, with the cursor the call updated. Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (rcvid, destination iov, offset, REPLY/SEND flag, cursor)
 * Return value: ENOERROR (part count reported via KSTATUS); EINVAL for a
 *               nonpositive part count or a short send the kernel buffered;
 *               ENOTSUP for a client iov array in another process; EINTR if
 *               the skip spent its budget before a part (call again); E2BIG
 *               if it did with no cursor; EFAULT if an iov entry cannot be
 *               read or the result cannot be written.
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

    if (kap->parts <= 0)
        return EINVAL;

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

    cop = reply_client_connection(thp, cop);
    if (!cop) {
        return ESRCH;
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
        /* A short send was copied into the thread's buffer and smsg/sparts were
         * not recorded (they hold a previous send's values), so there is no
         * client iov to describe.
         */
        if (thp->flags & RTMUX_FLG_THR_BUFF_MSG) {
            kspinlock_unlock(&cop->channel->lock);
            return EINVAL;
        }
        src_msg = thp->args.ms.smsg;
        src_parts = thp->args.ms.sparts;
    }

    /* viov_to_piov reads an iov array through the caller's address space, so it
     * can only walk one that lives there. A flat buffer (negative parts) is
     * described by the saved arguments alone and is translated in the client's
     * page table, so it works from any process.
     */
    if (src_parts > 0 && thp->process != act->process) {
        kspinlock_unlock(&cop->channel->lock);
        return ENOTSUP;
    }
    ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    kspinlock_unlock(&cop->channel->lock);

    n = viov_to_piov(act, thp->process, kap->iov, kap->parts, src_msg,
                     src_parts, kap->offset,
                     (kap->flags & RTMUX_READIOV_CURSOR) ? kap->cursor : NULL);

    /* Claim release + MSG_FORCE_RDY drain under channel_slock. */
    kspinlock_lock(&cop->channel->lock);
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
    if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
        ITF_CLR(thp, RTMUX_ITF_MSG_FORCE_RDY);
        thp_status = (unsigned) KSTATUS(thp);
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

    cop = reply_client_connection(thp, cop);
    if (!cop) {
        return ESRCH;
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
