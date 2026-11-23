/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include "reply-client.h"


/* The identity of the request an unblock notification is being made for
 * (force_ready names it for the length of its notification_deliver call), or 0
 * for any other notification. An unblock notification names an rcvid, which the
 * client's next request has too: the identity goes with the notification to the
 * server thread that takes it, and two notifications for different requests are
 * not folded into one.
 */
static uint64_t notification_request(int code)
{
    return code == _NOTIFICATION_CODE_UNBLOCK
               ? pcpu_array[get_cpunum()].pc_unblock_gen
               : 0;
}

/* The send queue contains both threads and notifications. A kernel terminal
 * signal carries a priority loan that ordinary notifications cannot borrow.
 */
static bool notification_matches(const notification_t *pup,
                                 bool terminal_signal,
                                 int code,
                                 intptr_t id)
{
    if (!pup)
        return false;
    unsigned type = TYPE_MASK(pup->type);
    return (type == TYPE_NOTIFICATION || type == TYPE_VNOTIFICATION) &&
           pup->terminal_signal == terminal_signal &&
           pup->code == (int8_t) code && pup->id == id;
}

/* Add (or compress/replace) a notification on a priority-ordered queue.
 *
 * For _NOTIFICATION_PRIO_REPLACE, overwrites a matching notification's value;
 * otherwise coalesces into an identical adjacent notification by bumping its
 * count, or allocates and enqueues a new notification. Parameters:
 *   prp      : process billed for a newly allocated notification soul
 *   queue    : the priority-ordered notification queue
 *   priority : notification priority, OR'd with _NOTIFICATION_PRIO_* control
 * bits code : notification code value : notification value id : notification id
 * (source connection) Return value: the added/updated notification, or NULL on
 * allocation failure.
 */
notification_t *rdecl notification_add(process_t *prp,
                                       pril_head_t *queue,
                                       unsigned priority,
                                       int code,
                                       uintptr_t value,
                                       intptr_t id)
{
    notification_t *pup;
    bool terminal_signal = (priority & _NOTIFICATION_PRIO_SIGNAL) != 0;

    if ((unsigned) priority & _NOTIFICATION_PRIO_REPLACE) {
        pup = pril_find_insertion(
            queue, (unsigned) priority | _NOTIFICATION_PRIO_HEAD);
        /* Scan the queue and replace if priority and code match. */
        while ((pup) && (pup->priority == (uint8_t) priority)) {
            if (notification_matches(pup, terminal_signal, code, id)) {
                pup->value = value;
                pup->gen = notification_request(code);
                overrun = 1;
                return pup;
            }
            pup = pril_next(pup);
        }
    } else {
        pup = pril_find_insertion(queue, (unsigned) priority);
        if (notification_matches(pup, terminal_signal, code, id) &&
            (pup->value == value) && pup->gen == notification_request(code) &&
            (pup->count < _NOTIFICATION_COUNT_MAX)) {
            /* We can compress this notification */
            ++pup->count;
            return pup;
        }
    }

    /* Allocate a notification entry. */
    pup = object_alloc(prp, &notification_souls);
    if (!pup) {
        return NULL;
    }
    /* Initialize it. */
    if ((unsigned) priority & _NOTIFICATION_PRIO_VTID) {
        pup->type = TYPE_VNOTIFICATION;
    } else {
        pup->type = TYPE_NOTIFICATION;
    }
    if ((unsigned) priority & _NOTIFICATION_PRIO_BOOST) {
        pup->type |= TYPE_FLAG_FIRST;
    }
    pup->terminal_signal = terminal_signal;
    pup->count = 1;
    pup->priority = (uint8_t) priority;
    pup->code = (int8_t) code;
    pup->value = value;
    pup->id = id;
    pup->gen = notification_request(code);

    /* Enqueue it. */

    /* FUTURE: pril_find_insertion() could have returned some information about
     * the PRIL state that would have made it possible to call a different
     * routine from pril_add() making use of the info and doing the insertion
     * faster. Something for another day....
     */
    pril_add(queue, pup);

    return pup;
}


/* Add a notification for an allocation-free realtime source (IRQ/timer).
 *
 * Unlike notification_add, this scans the whole queue for the source's queued
 * entry instead of only the priority-bucket tail, so a source that is queued
 * behind a same-priority peer still coalesces into one entry rather than
 * allocating a second. That one-entry-per-source guarantee is what makes the
 * single reservation each source took at attach/create sufficient, so a queued
 * fire never allocates. Any allocation it does perform draws from the reserve
 * (object_alloc_reserved), which object_reserve pre-grew, so the fire path is
 * allocation-free even under memory pressure.
 *
 * A source keeps its (code,value,id) identity across fires, but its resolved
 * priority can change (SIGEV priority -1 tracks the process priority), so a
 * priority change reinserts the queued entry rather than allocating. At the
 * count ceiling the entry saturates: the fire is folded (deterministic
 * queue-full), never dropped and never a second allocation. Parameters mirror
 * notification_add.
 *
 * Return value: the added/updated notification, or NULL only if the reserve is
 * unexpectedly exhausted.
 *
 * The scan is O(send_queue length) under chp->lock, so delivery is allocation-
 * free but not O(1); it is bounded by that channel's queued-sender-plus-
 * notification depth. If a future hard-RT bound needs O(1), give each source a
 * back-pointer to its queued notification instead of scanning.
 */
notification_t *rdecl notification_add_reserved(process_t *prp,
                                                pril_head_t *queue,
                                                unsigned priority,
                                                int code,
                                                uintptr_t value,
                                                intptr_t id)
{
    notification_t *pup;
    bool terminal_signal = (priority & _NOTIFICATION_PRIO_SIGNAL) != 0;

    for (pup = pril_first(queue); pup; pup = pril_next(pup)) {
        if (notification_matches(pup, terminal_signal, code, id) &&
            pup->value == value && pup->gen == notification_request(code)) {
            if (pup->priority != (uint8_t) priority) {
                pril_rem(queue, pup);
                pup->priority = (uint8_t) priority;
                pril_add(queue, pup);
            }
            if (pup->count < _NOTIFICATION_COUNT_MAX)
                ++pup->count;
            return pup;
        }
    }

    pup = object_alloc_reserved(prp, &notification_souls);
    if (!pup)
        return NULL;
    if ((unsigned) priority & _NOTIFICATION_PRIO_VTID)
        pup->type = TYPE_VNOTIFICATION;
    else
        pup->type = TYPE_NOTIFICATION;
    if ((unsigned) priority & _NOTIFICATION_PRIO_BOOST)
        pup->type |= TYPE_FLAG_FIRST;
    pup->terminal_signal = terminal_signal;
    pup->count = 1;
    pup->priority = (uint8_t) priority;
    pup->code = (int8_t) code;
    pup->value = value;
    pup->id = id;
    pup->gen = notification_request(code);
    pril_add(queue, pup);
    return pup;
}


/* Deliver a notification to a channel.
 *
 * If a receiver is waiting, transfers the notification data (inline or
 * buffered) and readies it; otherwise queues the notification on the channel
 * send_queue and boosts the server's threads to avoid priority inversion.
 *
 * notification_deliver brackets each chp->{receive,send}_queue access in the
 * per-channel lock. The lock is dropped around xfernotification (can fault on
 * user pages), around the inner ready() call (lock-rank SCHED < CHANNEL), and
 * around the priority-inheritance walk (does not touch channel queues). Callers
 * MUST NOT hold channel_slock on entry.
 *
 * A tthread_rcu section spans the receive-thread branch: channel_slock is
 * dropped between the chp->receive_queue read and the LINKPRIL_REM that unlinks
 * thp, and in that window a concurrent thread_destroy could force_ready thp,
 * vector_rem it, rcu_sched_synchronize() and object_free, which memsets the
 * soul, zeroing thp->prev. Holding the RCU section open blocks the writer's
 * rcu_sched_synchronize until we are done. Under RCU-sched, nested helper
 * sections are harmless compiler barriers and writer-side waiting is bounded by
 * peer kcall quiescence. Parameters:
 *   chp         : destination channel
 *   priority    : notification priority (low byte), OR'd with
 * _NOTIFICATION_PRIO_* bits code : notification code value : notification value
 * id : notification id (source side-channel scoid)
 *   notification_flags : NOTIFICATION_DELIVER_*_FLAG bits
 * Return value: EOK/EFAULT on delivery to a waiter, EOK when queued, or an
 *               errno: EINVAL (bad priority), ESRCH (channel being destroyed),
 *               ENXIO (process dying), EAGAIN (no notification soul).
 */
int rdecl notification_deliver(channel_t *chp,
                               unsigned priority,
                               int code,
                               uintptr_t value,
                               intptr_t id,
                               unsigned notification_flags)
{
    /* notification_flags are an or-ing of macros.h: NOTIFICATION_DELIVER_*_FLAG
     */
    thread_t *thp;
    unsigned prio = priority & 0xff;

    if ((prio == 0) || (prio > NUM_PRI - 1)) {
        return EINVAL;
    }
    /* Was there was a waiting thread on the channel? */
    rcu_read_lock(&tthread_rcu);

    /* Claim MSG_DELIVERY on the chosen thp before releasing channel_slock.
     * Without this claim, a concurrent ksys_msg_sendv on another CPU can pick
     * the same thp, do LINKPRIL_REM, and zero its prev/next via inline
     * block_and_ready; we would then re-acquire channel_slock at LINKPRIL_REM
     * and dereference thp->prev=0 (store fault). Mirrors the claim discipline
     * in ksys_msg_sendv and ksys_msg_sendnotification.
     */
    kspinlock_lock(&chp->lock);
    thp = chp->receive_queue;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    while ((thp) && (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY)) {
        thp = thp->next.thread;
    }

    /* Claim the chosen thp unconditionally — notification-only receivers
     * (RTMUX_ITF_RCVNOTIFICATION) are valid targets too and still take the
     * unlocked copy/LINKPRIL_REM path below, so they need the same claim
     * against a concurrent delivery on another CPU.
     */
    if (thp) {
        ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    }
#endif

    if (thp) {
        kspinlock_unlock(&chp->lock);

        /* Spin-wait if thp is mid-transition into STATE_RECEIVE. Pairs with the
         * receiver's WAIT_PENDING set+clear in ksys_msg_recv, bridging the
         * LINKPRIL_BEG -> unready window. The trailing fence pairs with the
         * writer's smp_mb-before-clear.
         */
        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
            __asm__ volatile("" ::: "memory");
        cpu_smp_mb();
        int status = 0;

        ITF_CLR(thp, RTMUX_ITF_RCVNOTIFICATION);
        thp->restart = NULL;
        /* Yes. Transfer the data. */
        if (((get_inkernel() & (INKERNEL_LOCK | INKERNEL_NOW)) ==
             INKERNEL_NOW) &&
            thp->as_prp == pcpu_array[get_cpunum()].pc_as_prp) {
            status =
                xfernotification(thp, thp->args.ri.rmsg, thp->args.ri.rparts,
                                 (uint32_t) code, value, (int32_t) id);
        } else {
            thp->args.ri.code = (int8_t) code;
            thp->args.ri.value = value;
            thp->args.ri.id = id;
            atomic_or((volatile unsigned *) &thp->flags,
                      RTMUX_FLG_THR_NOTIFICATION);
        }

        SETKSTATUS(thp, 0);

        if (status) {
            kcall_err(thp, EFAULT);
        } else {
            /* What thp holds now is this notification, unless the copy failed
             * and its receive with it. (A copy put off to its special return is
             * taken as made.)
             */
            msg_notification_received(thp, code, (int) value,
                                      notification_request(code));
        }

        /* Unlink receive thread from the receive queue and ready it.
         * MSG_DELIVERY claim was set above; keep it set across LINKPRIL_REM so
         * that even if force_ready races us in the STATE_RECEIVE branch, it'll
         * defer via force_ready_msgxfer. Clear immediately after LINKPRIL_REM
         * completes (thp is off the queue, future peeks won't find it anyway).
         */
        kspinlock_lock(&chp->lock);
        LINKPRIL_REM(thp);
        RECEIVER_TAKEN(thp);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        atomic_clr((volatile unsigned *) &thp->internal_flags,
                   RTMUX_ITF_MSG_DELIVERY);
#endif
        kspinlock_unlock(&chp->lock);

        /* By default the receiver runs with message driven priority: the
         * notification lends prio until thp next receives. A fixed-priority
         * channel lends nothing and drops a loan an earlier receive on another
         * channel left, as notification_lend does. thp is on no queue until
         * ready() below.
         */
        thread_lend_msg(thp,
                        (chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) &&
                                !(priority & _NOTIFICATION_PRIO_SIGNAL)
                            ? 0
                            : prio,
                        false);
        thread_prio_settle(thp);

        if ((unsigned) priority & _NOTIFICATION_PRIO_VTID) {
            atomic_or(&thp->flags, RTMUX_FLG_THR_RCVINFO);

            /* If the caller set the priority to _NOTIFICATION_PRIO_VTID, then
             * they passed in the sender's thread_t object as an id (this is
             * done in remote_ipc_send)
             */
            thp->args.ri.thp = (thread_t *) id;
            thp->args.ri.cop = thp->args.ri.thp->blocked_on;

            /* indicate that there is a receiver depending on data in our
             * thread_t object
             */
            ITF_SET(((thread_t *) id), RTMUX_ITF_SPECRET_PENDING);
        }
        if (notification_flags & NOTIFICATION_DELIVER_CRITICAL_FLAG) {
            CRITICAL_MARK_THREAD(thp);
        }
        ready(thp);
        rcu_read_unlock(&tthread_rcu);
        return status ? EFAULT : EOK;
    }

    /* No receiver. chp->lock is still held from the probe and stays held until
     * the notification is queued: ksys_msg_receivev re-probes send_queue under
     * it before it sleeps, so dropping it here would let a receiver sleep past
     * the notification.
     *
     * Refuse a dying process, and a channel being destroyed: channel_destroy
     * sets ->destroying under this lock before it drains send_queue.
     */
    if (chp->destroying ||
        (chp->process->flags & (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_ZOMBIE |
                                RTMUX_FLG_PROC_COREDUMP))) {
        /* ESRCH for a destroyed channel, as the send paths report it. */
        int err = chp->destroying ? ESRCH : ENXIO;

        kspinlock_unlock(&chp->lock);
        rcu_read_unlock(&tthread_rcu);
        return err;
    }

    /* The critical-delivery state is lost here. notification_add does not take
     * the lock itself (parallel to pril_add).
     */
    notification_t *added =
        (notification_flags & NOTIFICATION_DELIVER_RESERVED_FLAG)
            ? notification_add_reserved(chp->process, &chp->send_queue,
                                        priority, code, value, id)
            : notification_add(chp->process, &chp->send_queue, priority, code,
                               value, id);
    if (added) {
        kspinlock_unlock(&chp->lock);

        /* To prevent priority inversion, lend the pending notification to the
         * threads serving the channel until they next receive or reply;
         * critical-event state is not inherited.
         */
        if ((chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0)
            channel_lend_pending(chp, prio, true);

        rcu_read_unlock(&tthread_rcu);
        return EOK;
    }
    kspinlock_unlock(&chp->lock);

    rcu_read_unlock(&tthread_rcu);
    return EAGAIN;
}


/* Consume one count of a queued notification, freeing it when exhausted.
 * Parameters:
 *   prp   : process billed for the notification soul
 *   queue : queue the notification is on
 *   pup   : notification to decrement
 */
void rdecl notification_remove(process_t *prp,
                               pril_head_t *queue,
                               notification_t *pup)
{
    if (--pup->count == 0) {
        pril_rem(queue, pup);
        object_free(prp, &notification_souls, pup);
    }
}
