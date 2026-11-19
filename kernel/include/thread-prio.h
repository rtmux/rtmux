/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Base and effective thread priority.
 *
 * A thread has a base priority (thread_t.real_priority) that only its own
 * scheduling parameters change: SchedSet, and the sporadic server's drop to its
 * low priority and return. Other threads lend it priority:
 *
 *   - pi_loan:   the highest waiter on a PI mutex it owns, or the ceiling of a
 *                PRIO_PROTECT mutex it holds (kernel/sync/wait.c);
 *   - msg_loan:  the client whose request it is serving, or the notification
 *                it received last (kernel/ipc);
 *   - pend_loan: the highest send or notification still queued on the channel
 *                it serves (kernel/ipc).
 *
 * The scheduler runs it at the effective priority, the maximum of the four,
 * cached in thread_t.priority. Nothing else writes these fields;
 * scripts/check-prio-writes.sh fails `make check` on any write outside this
 * header and kernel/sync/wait.c. See docs/pi-pc-design.md "Base and Effective
 * Priority".
 *
 * msg_loan packs the priority (bits 0-7), a request flag (bit 8: lent by a
 * client in REPLY, as opposed to a notification) and a generation (bits 9-63)
 * that every new loan bumps. REPLY propagation updates the priority only with a
 * compare-and-swap on the generation it saw while the client was still the
 * server's client, so a late update can never overwrite a newer client's loan.
 */

#ifndef _KERNEL_THREAD_PRIO_H
#define _KERNEL_THREAD_PRIO_H

#ifdef KERNEL_INTERNAL

#define MSG_LOAN_PRIO_MASK 0xffULL
#define MSG_LOAN_REQUEST (1ULL << 8)
#define MSG_LOAN_GEN_ONE (1ULL << 9)

static inline unsigned msg_loan_prio(uint64_t word)
{
    return (unsigned) (word & MSG_LOAN_PRIO_MASK);
}

/* The base priority: the thread's own scheduling parameter. */
static inline unsigned thread_base_prio(const thread_t *thp)
{
    return thp->real_priority;
}

/* The largest priority lent to thp. A vthread (remote IPC proxy) is shorter
 * than thread_t and carries no loans.
 */
static inline unsigned thread_loan_prio(const thread_t *thp)
{
    unsigned loan, v;

    if (TYPE_MASK(thp->type) != TYPE_THREAD)
        return 0;
    loan = msg_loan_prio(__atomic_load_n(&thp->msg_loan, __ATOMIC_ACQUIRE));
    v = __atomic_load_n(&thp->pend_loan, __ATOMIC_ACQUIRE);
    if (loan < v)
        loan = v;
    v = __atomic_load_n(&thp->pi_loan, __ATOMIC_ACQUIRE);
    return loan > v ? loan : v;
}

/* The priority thp should be running at: max(base, loans). */
static inline unsigned thread_effective_prio(const thread_t *thp)
{
    unsigned base = __atomic_load_n(&thp->real_priority, __ATOMIC_ACQUIRE);
    unsigned loan = thread_loan_prio(thp);

    return base > loan ? base : loan;
}

/* The priority a thread created by thp starts at: the priority thp runs its own
 * work at. A server serving a request creates threads on its client's behalf
 * (roottask starting a spawned process's first thread), so that is the client's
 * priority, as POSIX inheritance from the spawner requires; otherwise it is
 * thp's base. PI and pending-send loans are never inherited.
 */
static inline unsigned thread_inherit_prio(const thread_t *thp)
{
    uint64_t loan;

    if (TYPE_MASK(thp->type) != TYPE_THREAD)
        return thp->real_priority;
    loan = __atomic_load_n(&thp->msg_loan, __ATOMIC_ACQUIRE);
    if ((loan & MSG_LOAN_REQUEST) && thp->client)
        return msg_loan_prio(loan);
    return thp->real_priority;
}

/* Set the base priority only. The caller then applies the new effective
 * priority (thread_prio_update).
 */
static inline void thread_set_base_prio(thread_t *thp, unsigned prio)
{
    __atomic_store_n(&thp->real_priority, (uint8_t) prio, __ATOMIC_RELEASE);
}

/* Initialise a full thread_t (never a vthread) that is on no queue yet: base
 * and effective priority prio, no loans. Its type may not be set yet.
 */
static inline void thread_init_prio(thread_t *thp, unsigned prio)
{
    thp->real_priority = (uint8_t) prio;
    thp->priority = (uint8_t) prio;
    thp->msg_loan = 0;
    thp->pend_loan = 0;
    thp->pi_loan = 0;
}

/* Start a new message loan (a new generation): prio lent by a client in REPLY
 * (request) or by a notification, or none (prio 0). The caller sets the
 * server's client link first, so a REPLY propagation that saw the old client
 * also sees an older generation and its compare-and-swap fails.
 */
static inline void thread_lend_msg(thread_t *thp, unsigned prio, bool request)
{
    uint64_t old, next;

    if (TYPE_MASK(thp->type) != TYPE_THREAD)
        return;
    old = __atomic_load_n(&thp->msg_loan, __ATOMIC_RELAXED);

    /* No loan to replace and none to start: a new generation fences off
     * nothing, since only a request loan can be relent.
     */
    if (prio == 0 && !(old & (MSG_LOAN_REQUEST | MSG_LOAN_PRIO_MASK)))
        return;
    do {
        next = ((old & ~(MSG_LOAN_GEN_ONE - 1)) + MSG_LOAN_GEN_ONE) |
               (request && prio ? MSG_LOAN_REQUEST : 0) |
               (prio & MSG_LOAN_PRIO_MASK);
    } while (!__atomic_compare_exchange_n(&thp->msg_loan, &old, next, false,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* End a notification loan: a SchedSet replaces the priority a received
 * notification lent, as it replaced the priority the receive wrote into the
 * base before loans were separate. A request loan stays, since its client still
 * waits on thp, and so does a PI or ceiling loan. The caller then applies the
 * new effective priority.
 */
static inline void thread_end_notification_loan(thread_t *thp)
{
    uint64_t old, next;

    if (TYPE_MASK(thp->type) != TYPE_THREAD)
        return;
    old = __atomic_load_n(&thp->msg_loan, __ATOMIC_RELAXED);
    do {
        if ((old & MSG_LOAN_REQUEST) || !(old & MSG_LOAN_PRIO_MASK))
            return;
        next = (old & ~(MSG_LOAN_GEN_ONE - 1)) + MSG_LOAN_GEN_ONE;
    } while (!__atomic_compare_exchange_n(&thp->msg_loan, &old, next, false,
                                          __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

/* The loan word, for a later thread_relend_msg(). */
static inline uint64_t thread_msg_loan_word(const thread_t *thp)
{
    return __atomic_load_n(&thp->msg_loan, __ATOMIC_ACQUIRE);
}

/* Change the priority of the request loan seen as `seen`, only if no newer loan
 * replaced it.
 *
 * Returns whether it did.
 */
static inline bool thread_relend_msg(thread_t *thp,
                                     uint64_t seen,
                                     unsigned prio)
{
    uint64_t next = (seen & ~MSG_LOAN_PRIO_MASK) | (prio & MSG_LOAN_PRIO_MASK);

    if (!(seen & MSG_LOAN_REQUEST))
        return false;
    return __atomic_compare_exchange_n(&thp->msg_loan, &seen, next, false,
                                       __ATOMIC_RELEASE, __ATOMIC_RELAXED);
}

/* End the request loan of the generation seen in `seen`, unless a newer loan
 * replaced it: a new generation with no priority. A REPLY propagation changing
 * only the priority meanwhile does not stop it.
 *
 * Returns whether it ended the loan. The caller then applies the new effective
 * priority (thread_prio_update, thread_prio_settle).
 */
static inline bool thread_end_msg_loan(thread_t *thp, uint64_t seen)
{
    uint64_t want = seen & ~MSG_LOAN_PRIO_MASK; /* flag and generation */

    if (!(seen & MSG_LOAN_REQUEST))
        return false;
    for (;;) {
        uint64_t next = (want & ~(MSG_LOAN_GEN_ONE - 1)) + MSG_LOAN_GEN_ONE;

        if (__atomic_compare_exchange_n(&thp->msg_loan, &seen, next, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return true;

        /* `seen` is now the current word: retry only if just the priority
         * changed.
         */
        if ((seen & ~MSG_LOAN_PRIO_MASK) != want)
            return false;
    }
}

/* End the request loan of a server that has no client left. A receive that made
 * a client its client installs the loan after it published the link, so a
 * client that left in between found no loan to end; the loan then stays until
 * this runs. The caller applies the new effective priority.
 *
 * The fence pairs with the one in thread_client_departed: the departing side
 * stores the client link and then loads the loan, this side stores the loan and
 * then loads the link, so with both fences at least one of them sees the
 * other's store and a loan installed after the client left is ended by one of
 * them. The generation alone cannot replace that: a notification lend or a
 * REPLY propagation also bumps it, so a loan install that failed on a
 * generation change could not tell a departed client from a busy server.
 *
 * Returns whether it ended a loan.
 */
static inline bool thread_end_orphan_loan(thread_t *srv)
{
    uint64_t now;

    cpu_smp_mb();
    now = thread_msg_loan_word(srv);
    if (__atomic_load_n(&srv->client, __ATOMIC_ACQUIRE) != NULL)
        return false;
    return thread_end_msg_loan(srv, now);
}

/* The one way a server stops serving `client`: take the client link, then end
 * the loan the client lent. It serves every departure: a timeout, signal or
 * channel destroy of the client (force_ready), the server naming another client
 * or none (MsgCurrent), and a receive that finds its client already gone. It
 * takes only atomics, so it is safe in timer and event context.
 *
 * Returns whether the loan ended, and so whether the server's priority must be
 * reapplied. A caller in thread context with no lock held does so with
 * thread_prio_update; one that may not (force_ready) calls
 * thread_prio_defer_settle.
 */
static inline bool thread_client_departed(thread_t *srv, thread_t *client)
{
    thread_t *expect = client;
    uint64_t seen;
    bool ended;

    /* Read the loan before the client link, as the REPLY walk does: a receive
     * sets the new client before it bumps the generation. A vthread carries no
     * loan.
     */
    seen = TYPE_MASK(srv->type) == TYPE_THREAD ? thread_msg_loan_word(srv) : 0;
    if (!__atomic_compare_exchange_n(&srv->client, &expect, NULL, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) ||
        TYPE_MASK(srv->type) != TYPE_THREAD)
        return false;
    ended = thread_end_msg_loan(srv, seen);
    ended |= thread_end_orphan_loan(srv);
    return ended;
}

/* Whether thp's message loan came from a client (it ends when the server has no
 * client left) rather than a notification (it ends at the next receive).
 */
static inline bool thread_msg_loan_is_request(const thread_t *thp)
{
    return (thread_msg_loan_word(thp) & MSG_LOAN_REQUEST) != 0;
}

/* Set the pending-send loan: the head of the queue thp serves, read under that
 * channel's lock.
 */
static inline void thread_set_pend_loan(thread_t *thp, unsigned prio)
{
    if (TYPE_MASK(thp->type) == TYPE_THREAD)
        __atomic_store_n(&thp->pend_loan, (uint8_t) prio, __ATOMIC_RELEASE);
}

/* Raise the pending-send loan to at least prio (a send was queued). */
static inline void thread_raise_pend_loan(thread_t *thp, unsigned prio)
{
    uint8_t old;

    if (TYPE_MASK(thp->type) != TYPE_THREAD)
        return;
    old = __atomic_load_n(&thp->pend_loan, __ATOMIC_RELAXED);
    while (old < prio && !__atomic_compare_exchange_n(
                             &thp->pend_loan, &old, (uint8_t) prio, false,
                             __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        ;
}

/* The loan a send queued on chp lends its server threads: the priority of the
 * queue head, or 0. Caller holds chp->lock.
 */
static inline unsigned channel_pending_loan_locked(channel_t *chp)
{
    pril_t *head;

    if (chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY)
        return 0;
    head = pril_first(&chp->send_queue);
    return head ? head->priority : 0;
}

/* Raise the pending-send loan to `pending` on every thread of chp's process
 * that last received on chp (a send or notification was queued there) and
 * reapply; with `walk`, also carry the change along what each is blocked in.
 * The caller holds no spinlock. Defined in kernel/sync/wait.c.
 */
void rdecl channel_lend_pending(channel_t *chp, unsigned pending, bool walk);

/* Set thp's pending-send loan again from the queue of the channel it last
 * received on, or to nothing when that channel is gone. A loan is otherwise
 * reset only by thp's own next receive or reply, so a thread that received once
 * and moved on keeps the priority of a send another thread has long since
 * taken. The caller holds no spinlock and then applies the new effective
 * priority. Defined in kernel/ipc/fastmsg.c.
 */
void rdecl thread_pend_loan_resettle(thread_t *thp);

/* Bring thp->priority to thread_effective_prio(thp) through adjust_priority,
 * then carry the change along the chain thp is blocked in: a PI mutex it waits
 * on, the server it is REPLY-blocked on, or the servers of the channel it is
 * SEND-blocked on, up to CONFIG_PRIO_REPLY_DEPTH IPC hops. The caller holds no
 * spinlock.
 *
 * Returns whether thp->priority changed. adjust_priority() ignores its
 * partition argument, so dpp is passed through for its callers' sake only.
 * Defined in kernel/sync/wait.c.
 */
bool rdecl thread_prio_update_dpp(thread_t *thp,
                                  kdispatch_t *dpp,
                                  int priority_inherit);

static inline void thread_prio_update(thread_t *thp)
{
    (void) thread_prio_update_dpp(thp, thp->dpp, 1);
}

/* A client left server srv and its loan ended (thread_client_departed) in a
 * context that may not take the PI lock or walk a wait chain (force_ready runs
 * from timer and event delivery under caller locks): queue srv on this hart's
 * list of servers whose priority is stale. The hart settles them, following the
 * chain each is blocked in as the REPLY propagation does, at its next kernel
 * exit, before it resumes or idles (thread_prio_run_deferred). srv need be
 * neither running nor ready: a server blocked on a second server is settled
 * too. Takes one leaf spinlock (LOCK_CLASS_PRIO_DEFER) and no other lock.
 */
void rdecl thread_prio_defer_settle(thread_t *srv);

/* Settle the servers queued on this hart, in thread context with no spinlock
 * held. Reapplies thread_effective_prio, so a settle another path did meanwhile
 * makes it a no-op.
 */
void rdecl thread_prio_run_deferred(void);

/* Take thp off its hart's queue before it is freed, after the RCU drain that
 * ends every departure still able to queue it and before the pin wait that ends
 * a settle already running on it.
 */
void rdecl thread_prio_defer_cancel(thread_t *thp);

/* Servers queued on each hart, for the kernel exit's cheap test. */
extern _Atomic unsigned prio_defer_pending[CONFIG_PROCESSORS_MAX];

/* Recheck after storing thp->priority directly on the IPC fast path, where thp
 * was on no queue: reapply if a concurrent loan change raced the store.
 */
void rdecl thread_prio_settle(thread_t *thp);

/* REPLY-propagation overflows: walks that stopped at CONFIG_PRIO_REPLY_DEPTH.
 * Exported in /sys/kstats as prio.reply_depth_overflow.
 */
extern _Atomic uint64_t prio_reply_depth_overflow;

#endif /* KERNEL_INTERNAL */

#endif /* _KERNEL_THREAD_PRIO_H */
