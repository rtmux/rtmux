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
