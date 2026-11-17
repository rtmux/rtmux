/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Synchronization-primitive kernel calls (slots 78..87 in
 * internal/include/sys/calls.h): SYNC_CREATE, SYNC_DESTROY, SYNC_MUTEX_LOCK,
 * SYNC_MUTEX_UNLOCK, SYNC_CONDVAR_WAIT, SYNC_CONDVAR_SIGNAL, SYNC_SEM_POST,
 * SYNC_SEM_WAIT, SYNC_CTL, SYNC_MUTEX_REVIVE.
 *
 * An alternative design — a per-sync "ksync_t" object, with every distinct user
 * "sync_t" getting a heap-allocated kernel object registered in a global
 * "sync_hash" and a priority-ordered wait list ("pril_head_t") embedded in it —
 * was rejected. In that scheme wakers walk "syp->waiting" and cross-CPU
 * synchronization between cv_wait and cv_signal relies on a
 * "RTMUX_ITF_WAIT_PENDING" bridge bit and busy-spinning walkers.
 *
 * The WAIT_PENDING bridge is unsafe under the BKL-less design: the cv_wait body
 * sets WAIT_PENDING, then unlocks the bound mutex, which internally
 * sync_wakeup's that mutex's wait list — and that nested sync_wakeup can
 * busy-spin on yet ANOTHER waiter's WAIT_PENDING. A concurrent cv_signal on a
 * third CPU, spinning on cv_wait's own WAIT_PENDING, completes a triangular
 * wait → system-wide deadlock.
 *
 * This implementation draws on FreeBSD's "kern/kern_umtx.c" — a design proven
 * on millions of SMP systems over twenty years. The borrowed ideas, in plain
 * terms:
 *
 * 1. Per-key wait queue, hash-indexed by user sync_t address. No heap object
 * per primitive: just a fixed-size table of "chains" and the user "sync_t"'s
 * memory itself for the lock word.
 *
 * 2. Per-chain spinlock instead of one global sync_slock. Lock contention
 * scales with the hash table size, not workload size.
 *
 * 3. Chain lock held across the userspace lock-word read AND the wait-queue
 * insert. That single critical section closes the POSIX cv_wait atomicity gap
 * (release-the-mutex-and-block) WITHOUT a bridge flag — concurrent cv_signal
 * must take the same chain lock to observe waiters, so it sees the waiter
 * cleanly inserted.
 *
 * 4. NO recursive sync_wakeup inside the cv_wait body. cv_wait releases the
 * bound mutex via the same kernel primitive used by mutex_unlock kcall, and the
 * mutex's wait queue is woken naturally — no nested critical-section walking.
 *
 * 5. thread_t's existing "next.thread" / "prev.thread" doubly-linked slots
 * (PRIL_ENTRY_FIELDS, see objects.h:154) carry the chain queue linkage. These
 * slots are unused while a thread is in STATE_MUTEX / STATE_CONDVAR / STATE_SEM
 * (the channel send/reply uses overlap on STATE_SEND / STATE_REPLY — disjoint).
 * Mutex contenders additionally publish blocked_on_turnstile so PI propagation
 * does not have to reuse blocked_on.
 *
 * Implementation status:
 *
 * * IMPLEMENTED: mutex_lock, mutex_unlock, cv_wait, cv_signal/broadcast,
 * sem_wait, sem_post, sync_create, sync_destroy. Wakes are priority ordered per
 * key. PRIO_INHERIT mutex contention creates a per-key turnstile, copies the
 * observed owner identity into kernel state, and applies bounded priority
 * inheritance. PRIO_PROTECT mutexes enter through the kernel path for ceiling
 * validation and boost/deboost. Robust mutexes enter it too, so that the kernel
 * records their holder and reports EOWNERDEAD when it dies (ROBUST MUTEXES
 * below).
 *
 * * STUBBED (return ENOERROR / EINVAL as appropriate; safe but functionally
 * incomplete): sync_ctl, sync_mutex_revive (mutex ownership-revival without a
 * record of the holder; refused for robust mutexes).
 *
 * * SPECIAL-RETURN SHIM: sync_mutex_lock is still exported for special-return.c
 * (ACQUIRE_MUTEX handling) — it calls into the chain-based implementation.
 *
 * * The "ksync_t" struct (objects.h) is no longer instantiated.
 * "thp->blocked_on" stays NULL for all sync waits so force-ready paths skip
 * ksync_t hold-list surgery. "thp->args.mu.wait_key" records the exact user
 * sync_t key while a thread is chain-queued. Wakers filter by that key, so
 * unrelated primitives that collide in the same hash chain do not share wait
 * queues.
 *
 * Locking discipline:
 *
 * - Per-chain kspinlock, rank LOCK_CLASS_TURNSTILE — BELOW SCHED in rank order
 * so that unready_release/block_release can take SCHED while the chain lock is
 * still held, then drop the chain lock at the precise moment pc_curthread has
 * been updated. This avoids the dispatch-double-entry hazard (see the block
 * comment in ksys_sync_mutex_lock's turnstile-block step).
 * - Chain lock is held briefly across queue mutations and adjacent atomic ops
 * on the user lock word. Scheduler handoff helpers may take SCHED while
 * releasing the chain lock.
 * - Two-chain operations (cv_wait: cv chain + mutex chain) acquire in address
 * order to avoid ABBA.
 *
 * Waker → waiter handshake:
 *
 * Waiter (cv_wait, mutex_lock slow path, sem_wait): chain_lock(C); atomic
 * re-check of sync state (POSIX double-check); enqueue self; mark thp->state =
 * STATE_*; chain_unlock(C); unready(thp, STATE_*); -- dispatcher picks new
 * active
 *
 * Waker (cv_signal, mutex_unlock, sem_post): chain_lock(C); atomic update of
 * sync state (clear owner / inc count); dequeue head waiter (if any); mark
 * waiter->state = STATE_READY; chain_unlock(C); ready(waiter);
 *
 * Critical invariant: state transition (RUNNING → STATE_*) and queue insert
 * happen under the chain lock. A concurrent waker that sees the waiter on the
 * queue is guaranteed to see state != RUNNING, so ready_default's INVARIANT-2
 * (state != RUNNING at ready time) holds without a WAIT_PENDING bridge. The
 * waiter calls unready AFTER releasing the chain lock — unready() reads state,
 * which has already been transitioned, and proceeds to block() with no extra
 * synchronization needed.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>

/* Chain hash table. */

#define SYNC_NCHAINS 128u
#define SYNC_CHAIN_MASK (SYNC_NCHAINS - 1u)
#define SYNC_PI_MAX_DEPTH 16u
_Atomic uint64_t prio_reply_depth_overflow;

struct sync_chain {
    kspinlock_t lock;
    thread_t *head; /* all waiters hashing to this chain, grouped by key */
    turnstile_t *turnstiles;
};

static struct sync_chain sync_chains[SYNC_NCHAINS];
static kspinlock_t sync_pi_slock;

static void sync_chain_requeue_waiter_locked(thread_t *thp,
                                             struct sync_chain *held);

struct turnstile_entry {
    sync_key_t key;
    struct turnstile_entry *next;
    thread_t *owner;
    struct turnstile_entry **owner_prevp;
    struct turnstile_entry *owner_next;
    unsigned long owner_word;
    int owner_tid;
    unsigned contenders;
    unsigned refs;
    unsigned max_waiter_prio;
    thread_t *bound; /* waiters bound but not yet queued, chain lock */
};

/* Physical address of a user VA in the current process (arch/riscv/cpu-pte.c).
 */
extern int cpu_uvaddr_to_paddr(uintptr_t vaddr, paddr_t *pa);

/* Resolve the wait-queue key for a user sync object: its physical address, so
 * the same physical lock word rendezvouses across every process that maps it
 * (and two processes' private syncs at the same virtual address get distinct
 * keys). The key is opaque — only hashed and compared, never dereferenced.
 *
 * Must run in the calling user thread's address space (a sync kcall) and BEFORE
 * the chain lock (the walk can fault; nothing may hold LOCK_CLASS_ TURNSTILE).
 * sync_probe() has already faulted the lock word in. as_slock is held only
 * across the page-table walk so a concurrent munmap cannot free a page-table
 * page under it; it is released before any chain lock, so it never nests under
 * LOCK_CLASS_TURNSTILE.
 *
 * Returns 0 with *key set, or -1 if the address no longer resolves (the mapping
 * vanished mid-operation — buggy user code); callers error the op (EFAULT)
 * rather than fall back to a virtual-address key, which could split a sync's
 * waiters across two keys.
 */
static int sync_resolve_key(sync_t *sync, sync_key_t *key)
{
    paddr_t pa;
    int rc;

    kspinlock_lock(&as_slock);
    rc = cpu_uvaddr_to_paddr((uintptr_t) sync, &pa);
    kspinlock_unlock(&as_slock);
    if (rc != 0)
        return -1;
    *key = (sync_key_t) pa;
    return 0;
}

/* Hash a wait-queue key into a chain index. The bottom 4 bits of a sync key are
 * always zero (a sync_t is 16 B aligned by ABI, and shared keys are page+16 B
 * aligned), so the hash shifts them out. A small bit-spread defeats trivial
 * stride patterns from arrays of mutexes.
 */
static inline unsigned sync_chain_idx(sync_key_t key)
{
    uintptr_t a = key;
    a >>= 4;
    a ^= a >> 9;
    return (unsigned) (a & SYNC_CHAIN_MASK);
}

/* Return the chain that owns the wait queue for "key". */
static inline struct sync_chain *sync_chain_of(sync_key_t key)
{
    return &sync_chains[sync_chain_idx(key)];
}

/* Lock two sync chains in canonical address order.
 *
 * Returns non-zero when both keys hash to the same chain, in which case the
 * single underlying kspinlock was acquired once. CONFIG_LOCK_DEBUG consumes the
 * explicit allowance below to reject ad hoc TURNSTILE same-class nesting
 * outside this helper.
 */
static int sync_lock_two(struct sync_chain *a, struct sync_chain *b)
{
    if (a == b) {
        kspinlock_lock(&a->lock);
        return 1;
    }
    if (a < b) {
        kspinlock_lock(&a->lock);
        lock_debug_allow_turnstile_pair();
        kspinlock_lock(&b->lock);
    } else {
        kspinlock_lock(&b->lock);
        lock_debug_allow_turnstile_pair();
        kspinlock_lock(&a->lock);
    }
    return 0;
}

/* Initialize the chain hash table. Called once at boot from kernel_init(): sets
 * up every chain's spinlock and empties its queue.
 */
void rdecl sync_init(void)
{
    kspinlock_init(&sync_pi_slock, LOCK_CLASS_TURNSTILE_OWNER, "sync_pi_slock");
    for (unsigned i = 0; i < SYNC_NCHAINS; i++) {
        kspinlock_init(&sync_chains[i].lock, LOCK_CLASS_TURNSTILE,
                       "sync_chain");
        sync_chains[i].head = NULL;
        sync_chains[i].turnstiles = NULL;
    }
}

/* Turnstile primitives.  Caller holds the owning chain lock unless noted. */

static turnstile_t *sync_turnstile_find_locked(struct sync_chain *c,
                                               sync_key_t key)
{
    for (turnstile_t *ts = c->turnstiles; ts; ts = ts->next) {
        if (ts->key == key)
            return ts;
    }
    return NULL;
}

static void sync_turnstile_remove_locked(struct sync_chain *c, turnstile_t *ts)
{
    turnstile_t **owner = &c->turnstiles;

    while (*owner) {
        if (*owner == ts) {
            *owner = ts->next;
            ts->next = NULL;
            return;
        }
        owner = &(*owner)->next;
    }
}

static void sync_turnstile_insert_locked(struct sync_chain *c,
                                         turnstile_t *ts,
                                         sync_key_t key)
{
    ts->key = key;
    ts->next = c->turnstiles;
    ts->owner = NULL;
    ts->owner_prevp = NULL;
    ts->owner_next = NULL;
    ts->owner_word = 0;
    ts->owner_tid = -1;
    ts->contenders = 0;
    ts->refs = 0;
    ts->max_waiter_prio = 0;
    c->turnstiles = ts;
}

static void sync_turnstile_ref_pi_locked(turnstile_t *ts)
{
    if (ts)
        ts->refs++;
}

static void sync_turnstile_ref(turnstile_t *ts)
{
    if (!ts)
        return;
    kspinlock_lock(&sync_pi_slock);
    sync_turnstile_ref_pi_locked(ts);
    kspinlock_unlock(&sync_pi_slock);
}

static void sync_pi_owner_list_remove_locked(turnstile_t *ts)
{
    if (!ts->owner_prevp)
        return;
    *ts->owner_prevp = ts->owner_next;
    if (ts->owner_next)
        ts->owner_next->owner_prevp = ts->owner_prevp;
    ts->owner = NULL;
    ts->owner_prevp = NULL;
    ts->owner_next = NULL;
}

static void sync_pi_owner_list_add_locked(thread_t *owner, turnstile_t *ts)
{
    ts->owner = owner;
    ts->owner_prevp = &owner->owned_turnstiles;
    ts->owner_next = owner->owned_turnstiles;
    if (ts->owner_next)
        ts->owner_next->owner_prevp = &ts->owner_next;
    owner->owned_turnstiles = ts;
}

/* The owner word carries only the low 15 pid bits (pid << 16, bit 31 is the
 * waiting flag), but pids are generation-tagged and exceed 0x7fff after eight
 * process exits. Compare only the bits the word can hold, or every owner whose
 * pid is that large looks ownerless and PI/deadlock detection silently stops.
 *
 * The truncation makes the word ambiguous across eight pid generations of one
 * process slot, and the tid check does not bound that (every process has a tid
 * 0). It only matters for a word left behind in shared memory by an owner that
 * died without releasing, and the effect is limited to a misplaced PI boost;
 * the full pid cannot be recovered from the 32-bit owner word without widening
 * it.
 */
static thread_t *sync_lookup_owner_pin(unsigned long owner_word)
{
    process_t *prp;
    thread_t *thp = NULL;
    pid_t pid;
    int id;
    int tid;

    owner_word &= ~RTMUX_SYNC_WAITING;
    if (owner_word == 0)
        return NULL;

    pid = (pid_t) (owner_word >> RTMUX_SYNC_OWNER_PID_SHIFT);
    tid = (int) (owner_word & 0xffffU) - 1;
    id = SYNC_PINDEX(owner_word);

    rcu_read_lock(&process_rcu);
    rcu_read_lock(&tthread_rcu);
    if (id && id < process_vector.nentries &&
        (prp = VECP(prp, &process_vector, id)) &&
        (prp->pid & RTMUX_SYNC_OWNER_PID_MASK) == pid) {
        thp = vector_lookup(&prp->threads, (unsigned) tid);
        if (thp && (thp->tid != (uint32_t) tid || tthread_pin(thp) != EOK))
            thp = NULL;
    }
    rcu_read_unlock(&tthread_rcu);
    rcu_read_unlock(&process_rcu);
    return thp;
}

/* Caller holds sync_pi_slock. Refresh owner->pi_loan, the priority its PI
 * waiters and PRIO_PROTECT ceilings lend it: the highest waiter on any
 * turnstile it owns, its held ceiling floor, and `floor` (a waiter bound to one
 * of its turnstiles but not yet queued there).
 *
 * Returns the effective priority, max(base, loans). Recompute is therefore
 * idempotent: it never deboosts a thread below a ceiling it still holds or a
 * message it still serves.
 */
static unsigned sync_pi_target_locked(thread_t *owner, unsigned floor)
{
    unsigned loan = floor;

    for (turnstile_t *ts = owner->owned_turnstiles; ts; ts = ts->owner_next) {
        if (loan < ts->max_waiter_prio)
            loan = ts->max_waiter_prio;
    }
    if (owner->held_ceiling_n != 0 && loan < owner->held_ceiling_floor)
        loan = owner->held_ceiling_floor;
    __atomic_store_n(&owner->pi_loan, (uint8_t) loan, __ATOMIC_RELEASE);
    return thread_effective_prio(owner);
}

/* Threads that sit on a sync chain; only these can have a meaningful
 * args.mu.wait_key (the args union is reused in every other state).
 */
static bool prio_on_sync_chain(const thread_t *thp)
{
    unsigned state = __atomic_load_n(&thp->state, __ATOMIC_ACQUIRE);

    return state == STATE_MUTEX || state == STATE_CONDVAR || state == STATE_SEM;
}

/* Bring thp->priority to thread_effective_prio(thp).
 *
 * The inputs (base and loans) are stored by other harts without a common lock,
 * so the read-and-adjust is serialized under sync_pi_slock, and every input
 * writer calls this after its store. The last of them to take the lock then
 * reads every store made before it and leaves the matching value; an earlier
 * one cannot overwrite it afterwards. The unlocked comparison only skips the
 * lock when nothing is to be done; a writer that skips it stored before the
 * comparison, so the holder of the lock, re-reading after its own adjust, sees
 * that store. The IPC fast path stores thp->priority directly into a thread on
 * no queue and then calls this (thread_prio_settle).
 *
 * adjust_priority takes the dispatch and, for a SEND or sync waiter, channel or
 * sync locks, all ranked above sync_pi_slock. The re-sort on the waiter's sync
 * chain needs a chain lock, ranked below, so it runs after the unlock; it reads
 * the priority under the chain lock and so files the thread by the latest
 * value.
 *
 * Returns whether the priority changed.
 */
static bool prio_apply(thread_t *thp,
                       kdispatch_t *dpp,
                       int priority_inherit,
                       struct sync_chain *held)
{
    bool changed = false;

    cpu_smp_mb(); /* order the caller's input store before the peek */
    if (thp->priority == thread_effective_prio(thp))
        return false;

    kspinlock_lock(&sync_pi_slock);
    for (;;) {
        unsigned target = thread_effective_prio(thp);

        if (thp->priority == target)
            break;
        _TRACE_SYS_EMIT_PI_BOOST(thp, thp->priority, target);
        adjust_priority(thp, target, dpp, priority_inherit);
        changed = true;
        cpu_smp_mb();
    }
    kspinlock_unlock(&sync_pi_slock);

    if (changed && prio_on_sync_chain(thp))
        sync_chain_requeue_waiter_locked(thp, held);
    return changed;
}

/* Where a blocked thread's priority is lent next over IPC. */
struct prio_ipc_hop {
    thread_t *srv;    /* REPLY: the server serving cur, pinned */
    uint64_t seen;    /* REPLY: srv->msg_loan while cur was its client */
    channel_t *chp;   /* SEND: the channel cur is queued on */
    unsigned pending; /* SEND: the head of chp's send queue */
};

/* Find the IPC hop for cur, which waits on no turnstile, under the lock of the
 * channel it waits on.
 *
 * Returns false when there is none.
 *
 * The client's blocked_on names its connection in STATE_SEND and STATE_REPLY,
 * and both states leave the channel's queue only under chp->lock, so with the
 * lock held and the state and blocked_on rechecked, args.ms is the live union
 * member (every other state reuses it) and args.ms.server is either NULL or a
 * server that has not finished tearing down: a server's teardown clears its
 * client's link before draining RCU, and this kcall is an RCU-sched reader. The
 * server must still name cur as its client. Its loan generation is read before
 * that check: a receive sets the new client before it bumps the generation, so
 * a generation read before the check is never newer than the client it saw.
 * WAIT_PENDING (a send or receive still moving cur between queues) is not
 * waited out; the transition it guards lends to the new server itself.
 */
static bool prio_ipc_hop_find(thread_t *cur, struct prio_ipc_hop *hop)
{
    connect_t *cop;
    channel_t *chp;
    unsigned state = __atomic_load_n(&cur->state, __ATOMIC_ACQUIRE);
    bool found = false;

    hop->srv = NULL;
    hop->chp = NULL;
    if (TYPE_MASK(cur->type) != TYPE_THREAD ||
        (state != STATE_REPLY && state != STATE_SEND) ||
        (cur->internal_flags & RTMUX_ITF_WAIT_PENDING))
        return false;
    cop = __atomic_load_n((connect_t **) &cur->blocked_on, __ATOMIC_ACQUIRE);
    if (!cop || (cop->flags & COF_NETCON) || !(chp = cop->channel))
        return false;

    kspinlock_lock(&chp->lock);
    if (cur->state != state || cur->blocked_on != cop ||
        (cur->internal_flags & RTMUX_ITF_WAIT_PENDING)) {
        /* moved on */
    } else if (state == STATE_SEND) {
        if ((chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
            hop->chp = chp;
            hop->pending = channel_pending_loan_locked(chp);
            found = true;
        }
    } else {
        thread_t *srv = cur->args.ms.server;

        if (srv && TYPE_MASK(srv->type) == TYPE_THREAD) {
            /* Acquire: the client check below reads after the word. */
            hop->seen = thread_msg_loan_word(srv);
            if (__atomic_load_n(&srv->client, __ATOMIC_ACQUIRE) == cur &&
                tthread_pin(srv) == EOK) {
                hop->srv = srv;
                found = true;
            }
        }
    }
    kspinlock_unlock(&chp->lock);
    return found;
}

void rdecl channel_lend_pending(channel_t *chp, unsigned pending, bool walk)
{
    process_t *prp = chp->process;
    thread_t *thp;

    if (!prp)
        return;
    for (unsigned i = 0; i < prp->threads.nentries; ++i) {
        if (VECP(thp, &prp->threads, i) && thp->last_chid == chp->chid &&
            __atomic_load_n(&thp->pend_loan, __ATOMIC_RELAXED) < pending) {
            thread_raise_pend_loan(thp, pending);
            if (walk)
                thread_prio_update(thp);
            else
                (void) prio_apply(thp, thp->dpp, 1, NULL);
        }
    }
}

static turnstile_t *sync_thread_blocked_ts_ref(thread_t *thp);
static void sync_turnstile_unref_deferred(turnstile_t *ts);
static thread_t *sync_turnstile_owner_pin(turnstile_t *ts);

/* Carry a priority change along the wait-for chain.
 *
 * The walk starts either at the turnstile `ts` that `waiter` is binding to (the
 * caller passes a reference it gives up), or at the pinned thread `cur` whose
 * priority just changed (the caller gives up the pin). Each step goes one of
 * three ways:
 *
 *   - a turnstile: boost its owner to max(its own target, waiter->priority)
 *     and continue with the turnstile the owner waits on, if any;
 *   - a REPLY-blocked thread: lend its priority to the server serving it (the
 *     request loan that server took at receive) and continue from that
 *     server, unless a newer loan replaced that one (the server no longer
 *     waits on behalf of this chain, so the walk ends);
 *   - a SEND-blocked thread: raise the pending loan of the threads serving
 *     its channel; the walk ends there.
 *
 * Every step continues even when nothing changes, so a cycle back to `waiter`
 * is still found. Bounds: SYNC_PI_MAX_DEPTH turnstiles, with a revisit or that
 * cap returning EDEADLK (a deadlock the contender must hear about), and
 * CONFIG_PRIO_REPLY_DEPTH IPC hops, past which the walk stops, leaves the
 * remaining threads as they were, and counts prio_reply_depth_overflow. An IPC
 * hop that reaches `waiter` is a cycle too and returns EDEADLK. No spinlock is
 * held from one step to the next.
 */
static int prio_chain_walk(thread_t *waiter, turnstile_t *ts, thread_t *cur)
{
    turnstile_t *visited[SYNC_PI_MAX_DEPTH];
    unsigned floor = waiter ? waiter->priority : 0;
    unsigned depth = 0;
    unsigned hops = 0;
    int result = EOK;

    while (ts || cur) {
        struct prio_ipc_hop hop;
        unsigned prio;
        bool lent;

        if (ts) {
            thread_t *owner;
            bool stop = depth == SYNC_PI_MAX_DEPTH;

            for (unsigned i = 0; i < depth && !stop; i++)
                stop = visited[i] == ts; /* revisit: a cycle */
            if (stop) {
                sync_turnstile_unref_deferred(ts);
                result = EDEADLK;
                break;
            }
            visited[depth++] = ts;

            owner = sync_turnstile_owner_pin(ts);
            ts = NULL;
            if (!owner)
                break;
            if (owner == waiter) {
                tthread_unpin(owner);
                result = EDEADLK;
                break;
            }

            kspinlock_lock(&sync_pi_slock);
            (void) sync_pi_target_locked(owner, floor);
            kspinlock_unlock(&sync_pi_slock);
            (void) prio_apply(owner, owner->dpp, 1, NULL);

            ts = sync_thread_blocked_ts_ref(owner);
            if (ts) {
                tthread_unpin(owner);
                continue;
            }
            cur = owner;
        }

        /* cur is pinned and waits on no turnstile: follow its IPC wait. */
        if (!prio_ipc_hop_find(cur, &hop)) {
            tthread_unpin(cur);
            break;
        }
        if (++hops > CONFIG_PRIO_REPLY_DEPTH) {
            (void) atomic_fetch_add_explicit(&prio_reply_depth_overflow, 1,
                                             memory_order_relaxed);
            if (hop.srv)
                tthread_unpin(hop.srv);
            tthread_unpin(cur);
            break;
        }
        if (hop.chp) {
            /* channel_t outlives this kcall's RCU-sched section; its process
             * threads are walked unpinned, as the send path does.
             */
            channel_lend_pending(hop.chp, hop.pending, false);
            tthread_unpin(cur);
            break;
        }
        if (hop.srv == waiter) {
            tthread_unpin(hop.srv);
            tthread_unpin(cur);
            result = EDEADLK;
            break;
        }

        /* Lend cur's current priority as the request loan srv took from it. A
         * failed swap with the same generation is another walk for the same
         * client getting there first: retry with cur's priority as it is now,
         * so the later change is the one that stays. A different generation
         * means srv took a newer client or replied since; that loan is then the
         * one to keep, and srv no longer waits for cur's request, so the walk
         * ends: going on would lend `floor` to the owners downstream of a
         * server that no longer serves this chain.
         */
        lent = false;
        for (;;) {
            uint64_t now;

            prio = __atomic_load_n(&cur->priority, __ATOMIC_ACQUIRE);
            if (thread_relend_msg(hop.srv, hop.seen, prio)) {
                (void) prio_apply(hop.srv, hop.srv->dpp, 1, NULL);
                lent = true;
                break;
            }
            now = thread_msg_loan_word(hop.srv);
            if (!(hop.seen & MSG_LOAN_REQUEST) ||
                ((now ^ hop.seen) & ~MSG_LOAN_PRIO_MASK))
                break;
            hop.seen = now;
        }
        tthread_unpin(cur);
        cur = NULL;
        if (!lent) {
            tthread_unpin(hop.srv);
            break;
        }

        ts = sync_thread_blocked_ts_ref(hop.srv);
        if (ts) {
            tthread_unpin(hop.srv);
            continue;
        }
        cur = hop.srv;
    }

    for (unsigned i = 0; i < depth; i++) {
        sync_turnstile_unref_deferred(visited[i]);
    }
    return result;
}

/* Follow a priority change of thp along whatever it is blocked in: the PI
 * turnstile it waits on, or the channel it is SEND- or REPLY-blocked on. The
 * unlocked peeks only skip a thread blocked in none of them; the walk
 * revalidates under its own locks.
 */
static void prio_chain_from(thread_t *thp)
{
    turnstile_t *ts = NULL;
    unsigned state;

    if (__atomic_load_n(&thp->blocked_on_turnstile, __ATOMIC_ACQUIRE))
        ts = sync_thread_blocked_ts_ref(thp);
    if (ts) {
        (void) prio_chain_walk(NULL, ts, NULL);
        return;
    }
    state = __atomic_load_n(&thp->state, __ATOMIC_ACQUIRE);
    if ((state == STATE_REPLY || state == STATE_SEND) &&
        tthread_pin(thp) == EOK)
        (void) prio_chain_walk(NULL, NULL, thp);
}

bool rdecl thread_prio_update_dpp(thread_t *thp,
                                  kdispatch_t *dpp,
                                  int priority_inherit)
{
    if (!prio_apply(thp, dpp, priority_inherit, NULL))
        return false;
    prio_chain_from(thp);
    return true;
}

void rdecl thread_prio_settle(thread_t *thp)
{
    (void) prio_apply(thp, thp->dpp, 1, NULL); /* fences first */
}

static void sync_pi_recompute_owner_held(thread_t *owner,
                                         struct sync_chain *held)
{
    if (!owner)
        return;

    kspinlock_lock(&sync_pi_slock);
    (void) sync_pi_target_locked(owner, 0);
    kspinlock_unlock(&sync_pi_slock);

    /* Under a held chain lock (the condvar wait leg) the owners recomputed are
     * the caller and the thread it hands the mutex to; neither is IPC-blocked
     * or queued on a foreign chain, so there is no chain to follow, and
     * following one would nest chain locks out of order.
     */
    if (prio_apply(owner, owner->dpp, 1, held) && !held)
        prio_chain_from(owner);
}

static void sync_pi_recompute_owner(thread_t *owner)
{
    sync_pi_recompute_owner_held(owner, NULL);
}

/* After a failed contended lock: the walk may have lent the contender's
 * priority further down the chain than the owner, and the owner's own priority
 * need not have changed, so walk the chain again unconditionally to take the
 * contender's floor back out.
 */
static void sync_pi_recompute_chain(thread_t *owner)
{
    if (!owner)
        return;
    kspinlock_lock(&sync_pi_slock);
    (void) sync_pi_target_locked(owner, 0);
    kspinlock_unlock(&sync_pi_slock);
    (void) prio_apply(owner, owner->dpp, 1, NULL);
    prio_chain_from(owner);
}

static void sync_turnstile_set_owner_thread_locked(turnstile_t *ts,
                                                   thread_t *owner,
                                                   unsigned long owner_word,
                                                   int owner_pinned)
{
    thread_t *old_owner;
    int have_owner_pin = owner ? 1 : 0;

    owner_word &= ~RTMUX_SYNC_WAITING;
    if (owner && !owner_pinned && tthread_pin(owner) != EOK) {
        owner = NULL;
        have_owner_pin = 0;
    }

    kspinlock_lock(&sync_pi_slock);
    old_owner = ts->owner;
    if (old_owner != owner) {
        sync_pi_owner_list_remove_locked(ts);
        if (owner)
            sync_pi_owner_list_add_locked(owner, ts);
    } else if (owner && have_owner_pin) {
        tthread_unpin(owner);
    }
    ts->owner_word = owner_word;
    ts->owner_tid = (int) (owner_word & 0xffffU) - 1;
    kspinlock_unlock(&sync_pi_slock);

    if (old_owner && old_owner != owner)
        tthread_unpin(old_owner);
}

static void sync_turnstile_record_owner(turnstile_t *ts, unsigned long owner)
{
    thread_t *owner_thp;
    int stale_owner = 0;

    owner &= ~RTMUX_SYNC_WAITING;

    /* If we already track this exact owner word but lost the thread pointer,
     * the tid/pid may have been recycled onto a different thread. Don't re-pin
     * and boost a stranger — bail and leave the turnstile ownerless.
     */
    kspinlock_lock(&sync_pi_slock);
    stale_owner = !ts->owner && ts->owner_word == owner && ts->owner_tid >= 0;
    kspinlock_unlock(&sync_pi_slock);
    if (stale_owner)
        return;

    owner_thp = sync_lookup_owner_pin(owner);
    sync_turnstile_set_owner_thread_locked(ts, owner_thp, owner, 1);
}

static thread_t *sync_turnstile_owner_pin(turnstile_t *ts)
{
    thread_t *owner = NULL;

    if (!ts)
        return NULL;

    kspinlock_lock(&sync_pi_slock);
    owner = ts->owner;
    if (owner && tthread_pin(owner) != EOK)
        owner = NULL;
    kspinlock_unlock(&sync_pi_slock);
    return owner;
}

static void sync_turnstile_update_max_locked(struct sync_chain *c,
                                             turnstile_t *ts)
{
    unsigned max_prio = 0;

    for (thread_t *cur = c->head; cur; cur = cur->next.thread) {
        if (cur->args.mu.wait_key == ts->key && max_prio < cur->priority)
            max_prio = cur->priority;
    }
    for (thread_t *cur = ts->bound; cur; cur = cur->ts_bound_next) {
        if (max_prio < cur->priority)
            max_prio = cur->priority;
    }

    kspinlock_lock(&sync_pi_slock);
    ts->max_waiter_prio = max_prio;
    kspinlock_unlock(&sync_pi_slock);
}

/* Caller holds the chain lock. A PI lock binds to the turnstile, lends its
 * priority with the chain lock dropped, and only then queues. Until it queues
 * it is on the bound list, which the turnstile maximum counts: a recompute in
 * that window (a handoff, another waiter leaving) must not take the loan back
 * from the owner.
 */
static void sync_turnstile_bind_waiter(turnstile_t *ts, thread_t *thp)
{
    ts->contenders++;
    thp->ts_bound_next = ts->bound;
    ts->bound = thp;
    kspinlock_lock(&sync_pi_slock);
    thp->blocked_on_turnstile = ts;
    if (ts->max_waiter_prio < thp->priority)
        ts->max_waiter_prio = thp->priority;
    kspinlock_unlock(&sync_pi_slock);
}

/* Caller holds the chain lock. Take thp off ts's bound list; a queued waiter is
 * not on it, so a miss is normal.
 */
static void sync_turnstile_bound_remove_locked(turnstile_t *ts, thread_t *thp)
{
    for (thread_t **pp = &ts->bound; *pp; pp = &(*pp)->ts_bound_next) {
        if (*pp == thp) {
            *pp = thp->ts_bound_next;
            thp->ts_bound_next = NULL;
            return;
        }
    }
}

static void sync_turnstile_unbind_waiter_locked(struct sync_chain *c,
                                                thread_t *thp)
{
    turnstile_t *ts;

    kspinlock_lock(&sync_pi_slock);
    ts = thp->blocked_on_turnstile;
    thp->blocked_on_turnstile = NULL;
    kspinlock_unlock(&sync_pi_slock);

    if (ts) {
        if (ts->contenders == 0)
            crash();
        ts->contenders--;
        sync_turnstile_bound_remove_locked(ts, thp);
        sync_turnstile_update_max_locked(c, ts);
    }
}

static void sync_turnstile_free_idle_locked(struct sync_chain *c,
                                            turnstile_t *ts)
{
    if (!ts || ts->contenders != 0)
        return;

    kspinlock_lock(&sync_pi_slock);
    if (ts->owner || ts->refs != 0) {
        kspinlock_unlock(&sync_pi_slock);
        return;
    }
    kspinlock_unlock(&sync_pi_slock);

    sync_turnstile_remove_locked(c, ts);
    _sfree(ts, sizeof(*ts));
}

static void sync_turnstile_unref_deferred(turnstile_t *ts)
{
    if (!ts)
        return;

    kspinlock_lock(&sync_pi_slock);
    if (ts->refs == 0)
        crash();
    ts->refs--;
    kspinlock_unlock(&sync_pi_slock);
}

static turnstile_t *sync_thread_blocked_ts_ref(thread_t *thp)
{
    turnstile_t *ts;

    kspinlock_lock(&sync_pi_slock);
    ts = thp->blocked_on_turnstile;
    sync_turnstile_ref_pi_locked(ts);
    kspinlock_unlock(&sync_pi_slock);
    return ts;
}

static int sync_pi_propagate(thread_t *waiter, turnstile_t *start_ts)
{
    sync_turnstile_ref(start_ts);
    return prio_chain_walk(waiter, start_ts, NULL);
}

static void robust_owner_died(thread_t *thp);
static void mutex_owner_died_waiters(thread_t *thp, struct sync_chain *c);

void rdecl sync_thread_cleanup(thread_t *thp)
{
    robust_owner_died(thp);
    if (thp->ts_spare) {
        _sfree(thp->ts_spare, sizeof(*thp->ts_spare));
        thp->ts_spare = NULL;
    }
    for (unsigned i = 0; i < SYNC_NCHAINS; i++) {
        struct sync_chain *c = &sync_chains[i];

        kspinlock_lock(&c->lock);
        mutex_owner_died_waiters(thp, c);
        for (turnstile_t *ts = c->turnstiles; ts; ts = ts->next) {
            kspinlock_lock(&sync_pi_slock);
            int owned = ts->owner == thp;
            kspinlock_unlock(&sync_pi_slock);
            if (!owned)
                continue;
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
        }
        kspinlock_unlock(&c->lock);
    }
}

#ifdef CONFIG_DEBUG_INJECT
/* Debug builds: crash if thp is still linked on any sync wait chain. A thread
 * that leaves STATE_MUTEX, STATE_CONDVAR or STATE_SEM, or is freed, while still
 * on a chain leaves the chain head or a neighbour pointing into memory the next
 * user of its next/prev links reuses: later wakers then ready threads that wait
 * on nothing. what names the caller's check.
 */
void sync_chains_assert_not_queued(const thread_t *thp, const char *what)
{
    for (unsigned i = 0; i < SYNC_NCHAINS; i++) {
        struct sync_chain *c = &sync_chains[i];
        unsigned steps = 0;

        /* No chain lock: callers may hold one, and thp is not being enqueued
         * while it is readied or freed, so a racing walk cannot invent a hit.
         */
        for (thread_t *cur = c->head; cur && steps < 4096;
             cur = cur->next.thread, steps++) {
            /* A waiter's wake unlinks it and clears prev; a link left behind
             * keeps it, which tells a real leak from a racing walk.
             */
            if (cur != thp || !thp->prev.thread)
                continue;
            kprintf(
                "sync chain %u still holds thread %p (%s): tid %d state %u "
                "wait_key %#lx prev %p next %p ra %p\n",
                i, (const void *) thp, what, thp->tid, (unsigned) thp->state,
                (unsigned long) thp->args.mu.wait_key,
                (void *) thp->prev.thread, (void *) thp->next.thread,
                __builtin_return_address(0));
            thr_trace_dump(thp);
            crash();
        }
    }
}

/* A thread readied out of a sync wait state must already be off its chain. */
void sync_chains_assert_readied(const thread_t *thp)
{
    if (thp->state == STATE_MUTEX || thp->state == STATE_CONDVAR ||
        thp->state == STATE_SEM)
        sync_chains_assert_not_queued(thp, "readied");
}

#endif

/* Wait queue primitives (LINK2 on thread_t next.thread/prev.thread). */

/* Caller holds chain lock. */
static void sync_enqueue(struct sync_chain *c, sync_key_t key, thread_t *thp)
{
    thread_t *cur = c->head;
    thread_t **insertp = &c->head;

    thp->args.mu.wait_key = key;
    thp->blocked_on = NULL;
    IPC_BLK(thp);

    /* The physical chain list may contain unrelated sync keys. Preserve FIFO
     * order across equal priorities for this key, but insert before the first
     * lower-priority waiter for the same key. Wakers scan by key, so unrelated
     * colliding keys remain isolated.
     *
     * Containment: thp must not already be on this chain — re-inserting it
     * below would splice the list into a cycle (re-inserting the current head
     * makes its next point at itself), after which every chain walk spins
     * forever, wedging the kernel with no fault. prev.thread can NOT be used as
     * an "already queued" flag: it aliases pril/notification pointers via
     * PRIL_ENTRY_FIELDS, so a stale value is not proof of membership and
     * unlinking on it faults. So fold a duplicate check into this same walk and
     * splice thp out with valid chain pointers when found. This is a safety net
     * for a latent double-enqueue (a wakeup path leaving thp queued), not the
     * root fix — the leaking caller still needs to be found.
     */
    while (cur) {
        if (cur == thp) {
            thread_t *nxt = (thp->next.thread == thp) ? NULL : thp->next.thread;
            *insertp = nxt;
            if (nxt)
                nxt->prev.thread = insertp;
            thp->next.thread = NULL;
            thp->prev.thread = NULL;
            cur = nxt;
            continue;
        }
        if (cur->next.thread == cur) /* bound a pre-existing 1-node self-loop */
            break;
        if (cur->args.mu.wait_key == key && cur->priority < thp->priority)
            break;
        insertp = &cur->next.thread;
        cur = cur->next.thread;
    }

    thp->prev.thread = insertp;
    thp->next.thread = cur;
    if (cur)
        cur->prev.thread = &thp->next.thread;
    *insertp = thp;
}

static void sync_unlink_waiter_only(thread_t *thp)
{
    if (!thp->prev.thread)
        return;
    *(thp->prev.thread) = thp->next.thread;
    if (thp->next.thread)
        thp->next.thread->prev.thread = thp->prev.thread;
    thp->next.thread = NULL;
    thp->prev.thread = NULL;
}

/* Caller holds c->lock. Whether thp is linked on chain c. */
static bool sync_chain_holds_locked(struct sync_chain *c, thread_t *thp)
{
    for (thread_t *cur = c->head; cur; cur = cur->next.thread) {
        if (cur == thp)
            return true;
        if (cur->next.thread == cur) /* see sync_enqueue */
            break;
    }
    return false;
}

static void sync_chain_requeue_waiter_locked(thread_t *thp,
                                             struct sync_chain *held)
{
    sync_key_t key = thp->args.mu.wait_key;
    struct sync_chain *c;
    turnstile_t *ts;

    if (!key)
        return;

    c = sync_chain_of(key);
    if (held != c)
        kspinlock_lock(&c->lock);

    /* wait_key and prev.thread alias other state once thp has left the chain
     * (the args union and the PRIL links are reused), so neither proves thp is
     * queued here: find it on the chain before moving it.
     */
    if (thp->args.mu.wait_key == key && thp->prev.thread &&
        sync_chain_holds_locked(c, thp)) {
        sync_unlink_waiter_only(thp);
        sync_enqueue(c, key, thp);
        kspinlock_lock(&sync_pi_slock);
        ts = thp->blocked_on_turnstile;
        kspinlock_unlock(&sync_pi_slock);
        if (ts)
            sync_turnstile_update_max_locked(c, ts);
    }
    if (held != c)
        kspinlock_unlock(&c->lock);
}

/* Caller holds chain lock. */
static void sync_dequeue(struct sync_chain *c, thread_t *thp)
{
    if (!thp->prev.thread)
        return;
    sync_unlink_waiter_only(thp);
    thp->args.mu.wait_key = 0;
    sync_turnstile_unbind_waiter_locked(c, thp);
}

/* Caller holds chain lock. */
static int sync_has_waiter(struct sync_chain *c, sync_key_t key)
{
    for (thread_t *cur = c->head; cur; cur = cur->next.thread) {
        if (cur->args.mu.wait_key == key)
            return 1;
    }
    return 0;
}

/* Caller holds chain lock. The WAITING bit the lock word of the mutex at key is
 * owed: RTMUX_SYNC_WAITING when a thread other than `except` waits for it, else
 * 0. A waiter counts from the moment it binds to the turnstile, not only once
 * it is queued: a PI lock drops the chain lock between the two, and a word
 * rewritten in that window from the queued waiters alone would strand it. ts is
 * the mutex's turnstile (NULL for a non-PI mutex); `except` is a queued waiter
 * about to be dequeued, which still counts in ts->contenders.
 */
static unsigned long mutex_waiting_bit_locked(struct sync_chain *c,
                                              sync_key_t key,
                                              const turnstile_t *ts,
                                              const thread_t *except)
{
    /* A PI waiter is counted from its bind to its dequeue, so the turnstile
     * alone answers for the queued ones too.
     */
    if (ts)
        return ts->contenders > (except ? 1U : 0U) ? RTMUX_SYNC_WAITING : 0;
    for (thread_t *cur = c->head; cur; cur = cur->next.thread) {
        if (cur != except && cur->args.mu.wait_key == key)
            return RTMUX_SYNC_WAITING;
    }
    return 0;
}

/* Caller holds chain lock. Pop and return the highest-priority waiter for key.
 */
static thread_t *sync_dequeue_head(struct sync_chain *c, sync_key_t key)
{
    thread_t *best = NULL;

    for (thread_t *thp = c->head; thp; thp = thp->next.thread) {
        if (thp->args.mu.wait_key == key &&
            (!best || best->priority < thp->priority)) {
            best = thp;
        }
    }
    if (best)
        sync_dequeue(c, best);
    return best;
}

/* Remove a thread from the chain-keyed sync wait queue during force_ready()
 * paths (timeout, signal/cancel, destroy). Chain waiters identify their queue
 * by args.mu.wait_key; there is no other sync wait queue.
 *
 * force_ready reads thp->state without any lock, so thp may have been woken,
 * run and blocked somewhere else by the time this takes the chain lock. Every
 * field read here before the lock is then stale: args.mu.wait_key aliases the
 * message arguments and prev.thread a channel queue link. Only under the chain
 * lock does a sync state mean that the thread is queued: a waker unlinks it
 * (clearing prev) under that lock before it readies it, and a waiter stamps its
 * state under the same lock it queued with.
 *
 * Returns true when thp was queued and is now unlinked, so the caller readies
 * it. False means it is not queued: a waker unlinked it and is about to ready
 * it (the caller must wait for that, or the thread may be destroyed or move on
 * before the waker's ready), or it waits somewhere else, or not at all, so the
 * caller starts over from its state.
 *
 * A thread woken and queued again on the same key passes the test and is
 * unblocked: that unblock reaches its new wait, as if it had arrived just after
 * the thread blocked again, a valid order for an interrupt or a cancel.
 * Wait_key is only written under its own chain lock, so the read here is
 * consistent. The chain address is computed from the key read before the lock,
 * so the state read under the lock is ordered after that read: a thread seen
 * with message arguments in the union has already left the sync states.
 *
 * The queued test is O(1): under the chain lock a sync state with the matching
 * key and a back link that points at thp mean thp is linked here. Walking the
 * chain to prove it would add O(chain) to the interrupt-disabled window of
 * every timeout and interrupt unblock of a condvar or semaphore waiter.
 */
bool rdecl sync_chain_force_ready(thread_t *thp)
{
    sync_key_t key = thp->args.mu.wait_key;
    struct sync_chain *c;
    turnstile_t *ts;
    thread_t *owner;

    if (!key)
        return false;

    c = sync_chain_of(key);
    kspinlock_lock(&c->lock);
    if (!prio_on_sync_chain(thp) || thp->args.mu.wait_key != key ||
        !thp->prev.thread || *thp->prev.thread != thp) {
        kspinlock_unlock(&c->lock);
        return false;
    }
    kspinlock_lock(&sync_pi_slock);
    ts = thp->blocked_on_turnstile;
    kspinlock_unlock(&sync_pi_slock);
    owner = sync_turnstile_owner_pin(ts);
    sync_dequeue(c, thp);
    if (thp->state == STATE_MUTEX && !sync_has_waiter(c, key)) {
        if (ts)
            sync_turnstile_free_idle_locked(c, ts);
    }
    kspinlock_unlock(&c->lock);
    sync_pi_recompute_owner(owner);
    if (owner)
        tthread_unpin(owner);
    return true;
}

/* User sync_t probing and validation. */

/* Validate the user pointer and fault-in the sync_t.
 *
 * Returns 0 on success, non-zero error code on failure (caller should kcall_err
 * the thread and return ENOERROR).
 */
static int sync_probe(thread_t *act, sync_t *sync)
{
    /* amoor.w never succeeds on a misaligned address and the S-mode trap is not
     * recoverable through the xfer handler, so reject it up front.
     */
    if (!sync || ((uintptr_t) sync & (sizeof(unsigned) - 1)) ||
        !WITHIN_BOUNDARY((uintptr_t) sync, (uintptr_t) (sync + 1),
                         act->process->boundary_addr)) {
        return EFAULT;
    }
    /* SMP-safe, fault-safe touch: OR-ing 0 probes without changing the word. */
    if (xfer_atomic_or_user((volatile unsigned *) &sync->count, 0) ||
        xfer_atomic_or_user((volatile unsigned *) &sync->owner, 0))
        return EFAULT;
    return 0;
}

/* MUTEX */

/* mutex_try_acquire() results. The positive ones mean the caller now owns the
 * mutex; the negative ones map to an errno through mutex_try_errno().
 */
#define MUTEX_TAKEN 1          /* acquired */
#define MUTEX_TAKEN_DEAD 2     /* acquired a robust mutex whose owner died */
#define MUTEX_RECURSED 3       /* re-acquired by its owner, depth counted */
#define MUTEX_BUSY 0           /* owned by another thread */
#define MUTEX_DESTROYED (-1)   /* EINVAL */
#define MUTEX_RELOCK (-2)      /* EDEADLK: non-recursive re-lock */
#define MUTEX_UNRECOVERED (-3) /* ENOTRECOVERABLE */

static int mutex_try_errno(int rc)
{
    switch (rc) {
    case MUTEX_RELOCK:
        return EDEADLK;
    case MUTEX_UNRECOVERED:
        return ENOTRECOVERABLE;
    default:
        return EINVAL;
    }
}

static int mutex_robust(const sync_t *sync)
{
    return (sync->count & RTMUX_SYNC_ROBUST) != 0;
}

/* Try to acquire the mutex atomically.
 *
 * Returns one of the MUTEX_* results above.
 */
static int mutex_try_acquire(thread_t *act, sync_t *sync)
{
    unsigned long want = SYNC_OWNER(act);
    unsigned long owner = *(volatile unsigned long *) &sync->owner;

    if (owner == RTMUX_SYNC_INITIALIZER || owner == RTMUX_SYNC_MUTEX_FREE) {
        if (_mux_smp_cmpxchg_ul(&sync->owner, owner, want) == owner) {
            return MUTEX_TAKEN;
        }
        return MUTEX_BUSY; /* race lost, fall through to slow path */
    }

    /* NOTRECOVERABLE ends any mutex whose owner is gone for good (see ROBUST
     * MUTEXES and OWNER DEATH below). DEAD, the word that lets the next locker
     * take the mutex, exists only on robust mutexes; on any other it reads as
     * an owner.
     */
    if (owner == RTMUX_SYNC_NOTRECOVERABLE)
        return MUTEX_UNRECOVERED;
    if (owner == RTMUX_SYNC_DEAD && mutex_robust(sync))
        return _mux_smp_cmpxchg_ul(&sync->owner, owner, want) == owner
                   ? MUTEX_TAKEN_DEAD
                   : MUTEX_BUSY;
    /* Owner non-zero: check for non-mutex types */
    if (owner == RTMUX_SYNC_DESTROYED)
        return MUTEX_DESTROYED;
    if ((owner & ~RTMUX_SYNC_WAITING) == want) {
        if ((sync->count & RTMUX_SYNC_NONRECURSIVE) != 0)
            return MUTEX_RELOCK;
        /* Recursive acquire by same owner — count the recursion. */
        sync->count++;
        return MUTEX_RECURSED;
    }
    return MUTEX_BUSY;
}

/* A dead owner's depth is gone and the state it protected is suspect: drop the
 * depth and flag the mutex inconsistent, until pthread_mutex_consistent.
 */
static void mutex_mark_inconsistent(sync_t *sync)
{
    unsigned long count = (unsigned long) sync->count;

    sync->count = (long) ((count & ~rtmux_sync_depth_mask((long) count)) |
                          RTMUX_SYNC_INCONSISTENT);
}

/* Leave an inconsistent mutex for the next locker to take with EOWNERDEAD. */
static void mutex_set_dead(sync_t *sync)
{
    mutex_mark_inconsistent(sync);
    *(volatile unsigned long *) &sync->owner = RTMUX_SYNC_DEAD;
}

static int mutex_pi_enabled(sync_t *sync)
{
    return (sync->count & RTMUX_SYNC_PRIOINHERIT) != 0;
}

/* The PRIO_PROTECT ceiling lives in its own bits (RTMUX_SYNC_CEIL_MASK),
 * disjoint from the recursion depth, so it reads back identically whether the
 * mutex is free or held at any depth — no held-depth correction, and no window
 * against libc's user-space depth adjust.
 */
static unsigned mutex_prio_ceiling(sync_t *sync)
{
    unsigned long count = (unsigned long) sync->count;

    if ((count & RTMUX_SYNC_PRIOCEILING) == 0)
        return 0;
    return (unsigned) ((count & RTMUX_SYNC_CEIL_MASK) >> RTMUX_SYNC_CEIL_SHIFT);
}

static int mutex_prio_ceiling_check(thread_t *act, sync_t *sync)
{
    unsigned ceiling = mutex_prio_ceiling(sync);

    if (ceiling != 0 && act->real_priority > ceiling)
        return EINVAL;
    return EOK;
}

/* Record that `t` now holds a PRIO_PROTECT mutex of the given ceiling. Caller
 * must NOT hold sync_pi_slock. Stores the ceiling in the held-set (up to
 * HELD_CEILING_MAX); beyond that it latches saturation and only tracks the
 * running max, which the pop path keeps conservatively until the set drains.
 */
static void sync_ceiling_push(thread_t *t, unsigned ceiling)
{
    kspinlock_lock(&sync_pi_slock);
    if (!t->held_ceiling_saturated && t->held_ceiling_n < HELD_CEILING_MAX)
        t->held_ceilings[t->held_ceiling_n] = (uint8_t) ceiling;
    else
        t->held_ceiling_saturated = true;
    t->held_ceiling_n++;
    if (t->held_ceiling_floor < ceiling)
        t->held_ceiling_floor = (uint8_t) ceiling;
    kspinlock_unlock(&sync_pi_slock);
}

/* Drop one held PRIO_PROTECT mutex of the given ceiling from `t`, then
 * recompute the effective ceiling floor as the max of what remains — so an
 * out-of-order release lowers the boost to the next-highest held ceiling rather
 * than pinning it at the historical max. Caller must NOT hold sync_pi_slock.
 * While saturated the exact set is unknown, so the floor is held until the
 * count reaches zero and the tracking resets.
 */
static void sync_ceiling_pop(thread_t *t, unsigned ceiling)
{
    kspinlock_lock(&sync_pi_slock);
    if (t->held_ceiling_n == 0) {
        kspinlock_unlock(&sync_pi_slock);
        return;
    }

    if (t->held_ceiling_saturated) {
        if (--t->held_ceiling_n == 0) {
            t->held_ceiling_saturated = false;
            t->held_ceiling_floor = 0;
        }
        kspinlock_unlock(&sync_pi_slock);
        return;
    }

    /* Exact path: swap-remove one entry equal to `ceiling`, then rescan the max
     * of the (small, bounded) remaining set.
     */
    unsigned n = t->held_ceiling_n;
    bool found = false;
    for (unsigned i = 0; i < n; i++) {
        if (t->held_ceilings[i] == ceiling) {
            t->held_ceilings[i] = t->held_ceilings[n - 1];
            found = true;
            break;
        }
    }
    t->held_ceiling_n = --n;

    /* `ceiling` not present means the tracked set no longer matches reality —
     * e.g. pthread_mutex_setprioceiling() changed a held mutex's ceiling in
     * user space between the boost (pushed the old value) and this unboost
     * (pops the new one). Recomputing the max would risk dropping a still-held
     * higher ceiling, so stay conservative: keep the current floor and latch
     * saturation until the set fully drains (never under-protect).
     */
    if (!found) {
        if (n == 0) {
            t->held_ceiling_saturated = false;
            t->held_ceiling_floor = 0;
        } else {
            t->held_ceiling_saturated = true;
        }
        kspinlock_unlock(&sync_pi_slock);
        return;
    }

    unsigned floor = 0;
    for (unsigned i = 0; i < n; i++) {
        if (t->held_ceilings[i] > floor)
            floor = t->held_ceilings[i];
    }
    t->held_ceiling_floor = (uint8_t) floor;
    kspinlock_unlock(&sync_pi_slock);
}

/* Apply/lift the priority-ceiling boost on acquire/release. Both fold the
 * ceiling into the thread's effective-priority floor (sync_pi_target_locked)
 * and then recompute, so the boost composes with PI inheritance and with other
 * held ceilings instead of clobbering them. The recompute is the only place
 * priority is set, which is why these never lower a thread that PI or a higher
 * ceiling still holds up.
 */
static void mutex_prio_ceiling_boost(thread_t *act, sync_t *sync)
{
    unsigned ceiling = mutex_prio_ceiling(sync);

    if (ceiling == 0)
        return;
    sync_ceiling_push(act, ceiling);
    sync_pi_recompute_owner(act);
}

static void mutex_prio_ceiling_unboost(thread_t *act, sync_t *sync)
{
    unsigned ceiling = mutex_prio_ceiling(sync);

    if (ceiling == 0)
        return;
    sync_ceiling_pop(act, ceiling);
    sync_pi_recompute_owner(act);
}

/* ROBUST MUTEXES
 *
 * A robust mutex (RTMUX_SYNC_ROBUST in count) never takes the user-space fast
 * path: every acquire and every final release runs here, so the kernel knows
 * the holder and keeps a robust_hold for each mutex on the holder's robust_held
 * list. When the holder dies, robust_owner_died() hands each such mutex to its
 * highest-priority waiter, which returns from its lock with EOWNERDEAD and the
 * mutex held. With no waiter it stores RTMUX_SYNC_DEAD, and the next lock takes
 * the mutex with EOWNERDEAD instead. Either way the dead owner's depth is
 * dropped and count gains RTMUX_SYNC_INCONSISTENT, which
 * pthread_mutex_consistent() clears. A release that still finds it set stores
 * RTMUX_SYNC_NOTRECOVERABLE and fails every waiter, and every later lock, with
 * ENOTRECOVERABLE. Those states live in the lock word, so no record outlives
 * the hold it describes.
 *
 * A dying thread is torn down from whatever context destroys it, not from its
 * own address space, so the lock word is reached through the kernel direct map
 * at its physical key. The key is trusted only while a live mapping still leads
 * to it: the new owner's (its own address, left in args.mu.mutex while it
 * waits) or, with no waiter, the dead owner's (rec->uva). The walk and the
 * stores run under as_slock, which every page-table change takes, so the page
 * cannot be unmapped and reused under the store. If neither mapping holds, the
 * mutex is left as it is (the word keeps naming the dead owner). Most robust
 * mutexes one thread may hold at once; one more lock is EAGAIN.
 */
#define ROBUST_HELD_MAX 64u

struct robust_hold {
    struct robust_hold *next;
    sync_key_t key; /* the mutex's physical address */
    sync_t *uva;    /* the holder's virtual address of it */
};

/* A new zeroed turnstile, or NULL when out of memory (or injected). */
static turnstile_t *sync_ts_alloc(thread_t *act)
{
#ifdef CONFIG_DEBUG_INJECT
    if (inject_should_fail_turnstile(act->process))
        return NULL;
#endif
    return _scalloc(sizeof(turnstile_t));
}

/* The thread's idle turnstile, or a new one: a contended PI lock needs one, and
 * a condvar wait reserves it (see ts_spare in objects.h) so the reacquire on
 * wake cannot fail on an allocation. Zeroed either way. NULL when out of
 * memory.
 */
static turnstile_t *sync_ts_take(thread_t *act)
{
    turnstile_t *ts = act->ts_spare;

    if (ts) {
        act->ts_spare = NULL;
        return ts;
    }
    return sync_ts_alloc(act);
}

/* Give back a turnstile that was never queued anywhere: keep it as the spare,
 * or free it.
 */
static void sync_ts_put(thread_t *act, turnstile_t *ts)
{
    if (!ts)
        return;
    if (!act->ts_spare)
        act->ts_spare = ts;
    else
        _sfree(ts, sizeof(*ts));
}

/* Get a record for a robust lock of sync: the caller's spare, or a new one.
 * Returns EOK with *recp set, or an errno.
 */
static int robust_rec_get(thread_t *act,
                          sync_t *sync,
                          struct robust_hold **recp)
{
    struct robust_hold *rec = act->robust_spare;
    sync_key_t key;

    /* Bounds the list robust_detach walks under sync_pi_slock. */
    if (act->robust_nheld >= ROBUST_HELD_MAX)
        return EAGAIN;
    if (sync_resolve_key(sync, &key) != 0)
        return EFAULT;
    if (rec)
        act->robust_spare = NULL;
    else if (!(rec = _scalloc(sizeof(*rec))))
        return EAGAIN;
    rec->key = key;
    *recp = rec;
    return EOK;
}

/* Keep an unused record in act's spare slot.
 *
 * Returns it if the slot is taken, for the caller to free (after dropping any
 * chain lock). Only act itself calls this, so the slot needs no lock.
 */
static struct robust_hold *robust_rec_stash(thread_t *act,
                                            struct robust_hold *rec)
{
    if (!rec || act->robust_spare)
        return rec;
    act->robust_spare = rec;
    return NULL;
}

/* Give an unused record back to act's spare slot, or free it. */
static void robust_rec_put(thread_t *act, struct robust_hold *rec)
{
    rec = robust_rec_stash(act, rec);
    if (rec)
        _sfree(rec, sizeof(*rec));
}

/* Stands in for the list head of a thread whose list was taken for its death
 * (robust_owner_died). It matches no key and ends the list.
 */
static struct robust_hold robust_closed;

/* File rec on thp's list; uva is thp's address of the mutex. Fails, leaving rec
 * alone, if thp is being destroyed: its list is closed, and nothing filed on it
 * would be handed on. A caller that hands a mutex to a waiter files the record
 * first, under the mutex's chain lock, and must then pick another waiter or
 * leave the mutex dead; the dying thread's own robust_hand_on takes that same
 * chain lock, so it sees the mutex in its final state.
 */
static bool robust_attach(thread_t *thp, struct robust_hold *rec, sync_t *uva)
{
    bool ok;

    kspinlock_lock(&sync_pi_slock);
    ok = thp->robust_held != &robust_closed;
    if (ok) {
        rec->uva = uva;
        rec->next = thp->robust_held;
        thp->robust_held = rec;
        thp->robust_nheld++;
    }
    kspinlock_unlock(&sync_pi_slock);
    return ok;
}

/* Take thp's record for key off its list.
 *
 * Returns it, or NULL if thp holds no robust mutex at key.
 */
static struct robust_hold *robust_detach(thread_t *thp, sync_key_t key)
{
    struct robust_hold **pp;
    struct robust_hold *rec = NULL;

    kspinlock_lock(&sync_pi_slock);
    for (pp = &thp->robust_held; *pp; pp = &(*pp)->next) {
        if ((*pp)->key == key) {
            rec = *pp;
            *pp = rec->next;
            thp->robust_nheld--;
            break;
        }
    }
    kspinlock_unlock(&sync_pi_slock);
    return rec;
}

/* Start the release of robust mutex `sync` by act, which keeps key's record in
 * *recp: only the holder the kernel recorded may release it, else EPERM.
 * *unrecoverable says the mutex is still inconsistent, so the release makes it
 * unrecoverable instead of passing it on; the record is then already given
 * back.
 */
static int robust_release_begin(thread_t *act,
                                sync_t *sync,
                                sync_key_t key,
                                struct robust_hold **recp,
                                bool *unrecoverable)
{
    *recp = robust_detach(act, key);
    if (!*recp)
        return EPERM;
    *unrecoverable = (sync->count & RTMUX_SYNC_INCONSISTENT) != 0;
    if (*unrecoverable) {
        robust_rec_put(act, *recp);
        *recp = NULL;
    }
    return EOK;
}

/* A mutex made unrecoverable has no depth and no state bit left. */
static void mutex_clear_count(sync_t *sync)
{
    unsigned long count = (unsigned long) sync->count;

    sync->count = (long) (count & ~(rtmux_sync_depth_mask((long) count) |
                                    RTMUX_SYNC_INCONSISTENT));
}

/* Store RTMUX_SYNC_NOTRECOVERABLE in the lock word and clear the state bit and
 * depth. New lockers see the word and fail at once.
 */
static void mutex_store_unrecoverable(sync_t *sync)
{
    mutex_clear_count(sync);
    *(volatile unsigned long *) &sync->owner = RTMUX_SYNC_NOTRECOVERABLE;
}

/* Caller holds c->lock: fail every waiter of key with ENOTRECOVERABLE. Once the
 * lock word is NOTRECOVERABLE the queue only drains.
 */
static void mutex_fail_waiters_locked(struct sync_chain *c, sync_key_t key)
{
    thread_t *thp;

    while ((thp = sync_dequeue_head(c, key))) {
        kcall_err(thp, ENOTRECOVERABLE);
        ready(thp);
    }
}

/* Caller holds c->lock; the word of key is already NOTRECOVERABLE. Fail its
 * waiters and drop its idle turnstile.
 */
static void mutex_fail_all_locked(struct sync_chain *c, sync_key_t key)
{
    turnstile_t *ts;

    mutex_fail_waiters_locked(c, key);
    ts = sync_turnstile_find_locked(c, key);
    if (ts) {
        sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
        sync_turnstile_free_idle_locked(c, ts);
    }
}

static void mutex_make_unrecoverable(thread_t *act,
                                     sync_t *sync,
                                     sync_key_t key)
{
    struct sync_chain *c = sync_chain_of(key);
    unsigned long count = (unsigned long) sync->count;
    thread_t *old_owner;

    kspinlock_lock(&c->lock);
    old_owner = sync_turnstile_owner_pin(sync_turnstile_find_locked(c, key));
    mutex_store_unrecoverable(sync);
    mutex_fail_all_locked(c, key);
    kspinlock_unlock(&c->lock);
    sync_pi_recompute_owner(old_owner);
    if (old_owner)
        tthread_unpin(old_owner);
    if (count & RTMUX_SYNC_PRIOCEILING)
        mutex_prio_ceiling_unboost(act, sync);
}

/* Caller holds c->lock. Hand robust mutex `key` to the highest-priority waiter
 * that can own it: dequeue it and file rec on it. A waiter whose thread is
 * being destroyed is skipped and left queued for its own teardown to remove.
 *
 * Returns NULL if no waiter took it; the releaser then frees the mutex, as it
 * would with no waiter at all.
 */
static thread_t *robust_pick_owner(struct sync_chain *c,
                                   sync_key_t key,
                                   struct robust_hold *rec)
{
    for (thread_t *thp = c->head; thp; thp = thp->next.thread) {
        if (thp->args.mu.wait_key != key)
            continue;
        if (robust_attach(thp, rec, thp->args.mu.mutex)) {
            sync_dequeue(c, thp);
            return thp;
        }
    }
    return NULL;
}

/* Caller holds as_slock. The kernel alias of the sync_t that prp maps at uva,
 * if that mapping is writable and still leads to key; else NULL.
 */
static sync_t *robust_word_locked(process_t *prp, sync_t *uva, sync_key_t key)
{
    uintptr_t va = (uintptr_t) uva;
    paddr_t pa;

    /* A sync_t is 16-byte aligned by ABI, but sync_probe() only checks 4 bytes;
     * one that straddles a page has no single physical alias.
     */
    if (!prp || !uva || (va & (__PAGESIZE - 1)) > __PAGESIZE - sizeof(sync_t))
        return NULL;
    if (!(vm_ops->vaddrinfo(prp->pid, prp, va, &pa, NULL, VI_NORMAL) &
          PROT_WRITE) ||
        (sync_key_t) pa != key)
        return NULL;
    return (sync_t *) CPU_P2V(pa);
}

/* Caller holds as_slock. The kernel alias of the lock word at uva, if it is
 * still a robust mutex owned by dead_word. The record can outlive a
 * pthread_mutex_destroy, after which the page may hold anything.
 */
static sync_t *robust_word_if_dead(process_t *prp,
                                   sync_t *uva,
                                   sync_key_t key,
                                   unsigned long dead_word)
{
    sync_t *ks = robust_word_locked(prp, uva, key);

    if (ks && mutex_robust(ks) &&
        (ks->owner & RTMUX_SYNC_OWNER_MASK) == dead_word)
        return ks;
    return NULL;
}

/* Hand on one robust mutex whose holder, dead, is being destroyed; see ROBUST
 * MUTEXES.
 *
 * Returns rec if it is no longer needed, NULL if it went to the new owner. The
 * lock word must still name the dead thread and still be a robust mutex: the
 * record can outlive a pthread_mutex_destroy, after which the page may hold
 * anything.
 */
static struct robust_hold *robust_hand_on(thread_t *dead,
                                          struct robust_hold *rec)
{
    sync_key_t key = rec->key;
    struct sync_chain *c = sync_chain_of(key);
    unsigned long dead_word = SYNC_OWNER(dead);
    sync_t *dead_uva = rec->uva;
    unsigned ceiling = 0;
    thread_t *head = NULL;
    turnstile_t *ts;
    sync_t *ks = NULL;

    kspinlock_lock(&c->lock);
    ts = sync_turnstile_find_locked(c, key);

    /* The first waiter that is not itself dying takes the mutex. The record
     * goes on its list before the lock word changes, so that its own death
     * cannot find the word without the record.
     */
    for (thread_t *cand = c->head; cand && !head; cand = cand->next.thread) {
        if (cand->args.mu.wait_key != key ||
            !robust_attach(cand, rec, cand->args.mu.mutex))
            continue;

        kspinlock_lock(&as_slock);
        ks = robust_word_if_dead(cand->process, cand->args.mu.mutex, key,
                                 dead_word);
        if (ks) {
            ceiling = mutex_prio_ceiling(ks);
            mutex_mark_inconsistent(ks);
            *(volatile unsigned long *) &ks->owner =
                SYNC_OWNER(cand) | mutex_waiting_bit_locked(c, key, ts, cand);
            head = cand;
        }
        kspinlock_unlock(&as_slock);
        if (!head) {
            /* Not the dead thread's mutex after all: take the record back. */
            rec = robust_detach(cand, key);
            break;
        }
    }

    /* No waiter to take it (or only dying ones): leave it dead, for the next
     * locker to take with EOWNERDEAD.
     */
    if (!head && !ks && rec) {
        kspinlock_lock(&as_slock);
        ks = robust_word_if_dead(dead->process, dead_uva, key, dead_word);
        if (ks)
            mutex_set_dead(ks);
        kspinlock_unlock(&as_slock);
    }

    if (!ks || !head) {
        /* Mutex handed on to no one: a PI turnstile left without contenders is
         * freed (sync_thread_cleanup clears the dead owner otherwise).
         */
        if (ks && ts) {
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
        }
        kspinlock_unlock(&c->lock);
        return rec;
    }

    /* The new owner takes the turnstile, and with it the PI loan of the waiters
     * still queued (sync_dequeue refreshed its maximum), the way an unlock
     * hands it on.
     */
    sync_dequeue(c, head);
    if (ts)
        sync_turnstile_set_owner_thread_locked(ts, head, SYNC_OWNER(head), 0);
    kspinlock_unlock(&c->lock);

    if (ceiling)
        sync_ceiling_push(head, ceiling);
    sync_pi_recompute_owner(head);
    kcall_err(head, EOWNERDEAD);
    ready(head);
    return NULL;
}

/* thp is being destroyed: hand on every robust mutex it holds, and free its
 * records. Nothing else adds to a dead thread's list.
 */
static void robust_owner_died(thread_t *thp)
{
    struct robust_hold *rec;
    struct robust_hold *next;

    /* Take the whole list and close it, so that a hand-off that races with this
     * death either filed its record already (it is on the list, and the chain
     * lock in robust_hand_on orders us after it) or finds the list closed and
     * passes the mutex on.
     */
    kspinlock_lock(&sync_pi_slock);
    rec = thp->robust_held;
    thp->robust_held = &robust_closed;
    kspinlock_unlock(&sync_pi_slock);

    for (; rec && rec != &robust_closed; rec = next) {
        next = rec->next;
        rec = robust_hand_on(thp, rec);
        if (rec)
            _sfree(rec, sizeof(*rec));
    }
    if (thp->robust_spare) {
        _sfree(thp->robust_spare, sizeof(*thp->robust_spare));
        thp->robust_spare = NULL;
    }
}

/* OWNER DEATH
 *
 * The kernel keeps no record of who holds a mutex that is not robust, so its
 * owner's death passes unseen: the lock word goes on naming a thread that will
 * never unlock it, and every waiter would sleep for ever. Two checks close
 * that, both run under the mutex's chain lock against a thread already stamped
 * STATE_DEAD, which is set before sync_thread_cleanup runs:
 *
 * - a thread that dies scans the waiters for mutexes whose lock word names it
 *   (mutex_owner_died_waiters);
 * - a locker about to queue first checks that the word's owner is alive
 *   (mutex_owner_gone).
 *
 * Whichever comes second sees the other's effect, so a waiter is never missed.
 * The mutex is then left RTMUX_SYNC_NOTRECOVERABLE and its waiters, and every
 * later locker, fail with ENOTRECOVERABLE. A process-shared mutex is reached
 * the same way as a robust one, through the waiter's own mapping. A robust
 * mutex is left to its own hand-off (ROBUST MUTEXES), whose failure the scan
 * above still covers.
 *
 * An owner that dies after clearing its id in the user-space release but before
 * the kernel hand-off leaves only the waiting bit: its waiters are not found.
 * Closing that window takes a kernel record of the hold, which a non-robust
 * mutex deliberately does not have.
 *
 * The owner id in the lock word is truncated (RTMUX_SYNC_OWNER_PID_MASK), so a
 * dead owner's id that a live thread has since taken over reads as alive, and
 * later lockers wait as they would for that thread: that is the limit of an
 * owner id this narrow, and no worse than before this check.
 */

/* The special lock words (INITIALIZER, DESTROYED, the sem/cond/spin tags, DEAD,
 * NOTRECOVERABLE) all have 0xffffff in bits 8..31: they name no owner and carry
 * no waiting bit.
 */
static bool mutex_word_special(unsigned long word)
{
    return (word >> 8) == (RTMUX_SYNC_DEAD >> 8);
}

/* Caller holds the chain lock. Take back the WAITING bit a contended lock
 * published when no waiter is left to need it: none queued, none bound to the
 * turnstile. Cleared in place, since a load and store of the whole word could
 * write back an owner that unlocked, or locked, in user space in between. A
 * special word (the owner died meanwhile) has no waiting bit and must not lose
 * or gain one.
 *
 * Returns whether no waiter remains.
 */
static bool mutex_waiting_withdraw_locked(struct sync_chain *c,
                                          sync_key_t key,
                                          const turnstile_t *ts,
                                          sync_t *sync)
{
    if (mutex_waiting_bit_locked(c, key, ts, NULL))
        return false;
    if (!mutex_word_special(*(volatile unsigned long *) &sync->owner))
        atomic_clr_64((volatile unsigned long *) &sync->owner,
                      RTMUX_SYNC_WAITING);
    return true;
}

/* Whether the owner named by sync's lock word is gone: dead, or no longer
 * findable. Caller holds the chain lock. On true, *owner is the owner id the
 * word named, without the waiting bit.
 */
static bool mutex_owner_gone(sync_t *sync, unsigned long *owner)
{
    unsigned long word = *(volatile unsigned long *) &sync->owner;
    thread_t *thp;
    bool gone;

    /* The special words (FREE, DESTROYED, ...) name no owner. */
    if (mutex_word_special(word) || (word & RTMUX_SYNC_OWNER_MASK) == 0)
        return false;
    thp = sync_lookup_owner_pin(word);
    gone = !thp || __atomic_load_n(&thp->state, __ATOMIC_ACQUIRE) == STATE_DEAD;
    if (thp)
        tthread_unpin(thp);
    *owner = word & RTMUX_SYNC_OWNER_MASK;
    return gone;
}

/* Make sync unrecoverable if its lock word still names owner, which is gone.
 * The word is changed by compare-and-swap, because the user-space release is
 * not serialized by the chain lock: the owner we looked up can have released
 * the mutex, and another thread taken it, since the lookup. Waiting is the only
 * bit a locker may add meanwhile.
 *
 * Returns whether the mutex was made unrecoverable.
 */
static bool mutex_fail_dead_owner(sync_t *sync, unsigned long owner)
{
    unsigned long word = *(volatile unsigned long *) &sync->owner;

    while ((word & RTMUX_SYNC_OWNER_MASK) == owner) {
        unsigned long seen =
            _mux_smp_cmpxchg_ul(&sync->owner, word, RTMUX_SYNC_NOTRECOVERABLE);

        if (seen == word) {
            mutex_clear_count(sync);
            return true;
        }
        word = seen;
    }
    return false;
}

/* mutex_try_acquire() for a caller that holds c->lock and is about to queue: a
 * mutex that is busy because its owner is gone is made unrecoverable instead. A
 * robust mutex is left to its owner's death hand-off.
 */
static int mutex_try_acquire_locked(thread_t *act,
                                    sync_t *sync,
                                    struct sync_chain *c,
                                    sync_key_t key)
{
    for (;;) {
        unsigned long owner;
        int rc = mutex_try_acquire(act, sync);

        if (rc != MUTEX_BUSY || mutex_robust(sync) ||
            !mutex_owner_gone(sync, &owner))
            return rc;
        if (mutex_fail_dead_owner(sync, owner)) {
            mutex_fail_all_locked(c, key);
            return MUTEX_UNRECOVERED;
        }
        /* The word changed under us: look at it again. */
    }
}

/* Caller holds c->lock. thp, being destroyed, may hold mutexes that waiters on
 * this chain are blocked on: fail those mutexes. Each waiter reaches the lock
 * word through its own mapping. A mutex already failed is no longer named by
 * the dead owner, so a repeat attempt on the same word is a cheap no-op.
 */
static void mutex_owner_died_waiters(thread_t *thp, struct sync_chain *c)
{
    unsigned long dead_word = SYNC_OWNER(thp);
    bool failed;

    do {
        failed = false;
        for (thread_t *w = c->head; w; w = w->next.thread) {
            sync_key_t key = w->args.mu.wait_key;
            sync_t *ks;

            if (w->state != STATE_MUTEX)
                continue;
            kspinlock_lock(&as_slock);
            ks = robust_word_locked(w->process, w->args.mu.mutex, key);
            failed = ks && mutex_fail_dead_owner(ks, dead_word);
            kspinlock_unlock(&as_slock);
            if (failed) {
                /* w is readied here: leave the walk and start over. */
                mutex_fail_all_locked(c, key);
                break;
            }
        }
    } while (failed);
}

/* Finish a lock attempt that ended with a result other than MUTEX_BUSY.
 *
 * A failure is reported as its errno. A mutex taken from a dead owner is marked
 * inconsistent and reports EOWNERDEAD; the libc wrapper then counts the new
 * hold as usual. A robust mutex gets the caller's record unless this was a
 * recursive acquire, which already has one. Last, the ceiling boost.
 */
static void mutex_lock_done(thread_t *act,
                            sync_t *sync,
                            struct robust_hold **recp,
                            int rc)
{
    if (rc < 0) {
        kcall_err(act, mutex_try_errno(rc));
        return;
    }
    if (rc == MUTEX_TAKEN_DEAD) {
        mutex_mark_inconsistent(sync);
        kcall_err(act, EOWNERDEAD);
    }
    if (*recp && rc != MUTEX_RECURSED) {
        if (robust_attach(act, *recp, sync))
            *recp = NULL; /* else the caller frees it */
    }
    mutex_prio_ceiling_boost(act, sync);
}

/* The body of ksys_sync_mutex_lock, after the probe. A robust mutex comes with
 * a record (*recp) for the caller to keep if it acquires the mutex; whatever is
 * left in *recp afterwards is the caller's to release, except on the blocking
 * path, which parks it first (see below).
 */
static void mutex_lock_body(thread_t *act,
                            sync_t *sync,
                            struct robust_hold **recp)
{
    unsigned long want = SYNC_OWNER(act);
    turnstile_t *allocated_ts = NULL;
    turnstile_t *free_ts = NULL;
    struct robust_hold *free_rec = NULL;
    int pi_enabled;
    int rc;

    rc = mutex_prio_ceiling_check(act, sync);
    if (rc) {
        kcall_err(act, rc);
        return;
    }

    /* Uncontested fast path */
    rc = mutex_try_acquire(act, sync);
    if (rc != MUTEX_BUSY) {
        mutex_lock_done(act, sync, recp, rc);
        return;
    }

    /* Contested: resolve the physical key (before any chain lock), then queue
     * ourselves under the chain lock and block.
     */
    sync_key_t key;
    if (sync_resolve_key(sync, &key) != 0) {
        kcall_err(act, EFAULT);
        return;
    }
    struct sync_chain *c = sync_chain_of(key);
retry_contended:
    kspinlock_lock(&c->lock);

    /* Double-check: the holder may have released between the fast-path read and
     * the chain-lock acquire. Try once more under the lock.
     */
    rc = mutex_try_acquire_locked(act, sync, c, key);
    if (rc != MUTEX_BUSY) {
        kspinlock_unlock(&c->lock);
        sync_ts_put(act, allocated_ts);
        mutex_lock_done(act, sync, recp, rc);
        return;
    }

    /* Mark the lock word as having waiters. This is observed by mutex_unlock to
     * know it must wake someone.
     */
    inject_mutex_lock_early(act->process);
    atomic_or_64((volatile unsigned long *) &sync->owner, RTMUX_SYNC_WAITING);
    pi_enabled = mutex_pi_enabled(sync);
    turnstile_t *ts = NULL;
    if (pi_enabled) {
        ts = sync_turnstile_find_locked(c, key);
        if (!ts) {
            if (!allocated_ts) {
                kspinlock_unlock(&c->lock);
                allocated_ts = sync_ts_take(act);
                if (!allocated_ts) {
                    /* The bit published above is ours to take back: the holder
                     * may have released in user space since, leaving a free
                     * word that every trylock would read as busy.
                     */
                    kspinlock_lock(&c->lock);
                    (void) mutex_waiting_withdraw_locked(
                        c, key, sync_turnstile_find_locked(c, key), sync);
                    kspinlock_unlock(&c->lock);
                    kcall_err(act, EAGAIN);
                    return;
                }
                goto retry_contended;
            }
            ts = allocated_ts;
            allocated_ts = NULL;
            sync_turnstile_insert_locked(c, ts, key);
        } else if (allocated_ts) {
            free_ts = allocated_ts;
            allocated_ts = NULL;
        }
    }
    if (pi_enabled) {
        unsigned long lent_to = *(volatile unsigned long *) &sync->owner;

        sync_turnstile_record_owner(ts, lent_to);
        sync_turnstile_bind_waiter(ts, act);
        kspinlock_unlock(&c->lock);
        inject_mutex_lock_stall(act->process);
        rc = sync_pi_propagate(act, ts);
        inject_mutex_lock_propagated(act->process);
        kspinlock_lock(&c->lock);
        if (rc != EOK) {
            thread_t *owner = sync_turnstile_owner_pin(ts);

            sync_turnstile_unbind_waiter_locked(c, act);
            if (mutex_waiting_withdraw_locked(c, key, ts, sync)) {
                sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
                sync_turnstile_free_idle_locked(c, ts);
            }
            kspinlock_unlock(&c->lock);
            sync_pi_recompute_chain(owner);
            if (owner)
                tthread_unpin(owner);
            kcall_err(act, rc);
            sync_ts_put(act, free_ts);
            return;
        }

        rc = mutex_try_acquire_locked(act, sync, c, key);
        if (rc != MUTEX_BUSY) {
            sync_turnstile_unbind_waiter_locked(c, act);
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
            kspinlock_unlock(&c->lock);
            sync_ts_put(act, free_ts);
            mutex_lock_done(act, sync, recp, rc);
            return;
        }

        /* The owner may have changed in the window: a release in user space
         * with nobody queued frees the mutex, and another thread can take it
         * before this one relocks. The walk above lent to the old owner, so
         * take the new owner's loan from the turnstile maximum, which counts
         * this waiter while it is bound.
         */
        unsigned long now = *(volatile unsigned long *) &sync->owner;

        sync_turnstile_record_owner(ts, now);
        if ((now & ~RTMUX_SYNC_WAITING) != (lent_to & ~RTMUX_SYNC_WAITING)) {
            thread_t *owner = sync_turnstile_owner_pin(ts);

            sync_pi_recompute_owner_held(owner, c);
            if (owner)
                tthread_unpin(owner);
        }
        sync_turnstile_bound_remove_locked(ts, act);
    }

    /* Enqueue self, transition state under the chain lock, leaving our own
     * address of the mutex: a handoff to a robust waiter files its record under
     * it, and the owner's death reaches the lock word through it.
     */
    act->args.mu.mutex = sync;
    sync_enqueue(c, key, act);
    if (pi_enabled)
        sync_turnstile_update_max_locked(c, ts);

    /* A PI lock dropped the chain lock to propagate priority after the first
     * publish above. A handoff or a failed propagation in that window counts
     * this waiter as bound (mutex_waiting_bit_locked) and keeps the bit, but an
     * unlock that found nobody queued stores a free word, and a user-space
     * locker can take it, without the bit, before this waiter relocks;
     * republish it now that this waiter is queued. A non-PI lock never dropped
     * the lock. A special word (the owner died meanwhile; the death scan runs
     * under this chain lock) has no waiting bit and must not gain one.
     */
    if (pi_enabled &&
        !mutex_word_special(*(volatile unsigned long *) &sync->owner))
        atomic_or_64((volatile unsigned long *) &sync->owner,
                     RTMUX_SYNC_WAITING);

    /* Lost-wakeup guard (futex re-check). The holder releases the mutex
     * entirely in user space: pthread_mutex_unlock clears the owner-id bits of
     * the lock word (keeping RTMUX_SYNC_WAITING; see __mutex_release_owner in
     * cpucfg.h) and only enters ksys_sync_mutex_unlock when that release
     * observed RTMUX_SYNC_WAITING. The chain lock does NOT serialize that
     * user-side release. So between the under-lock mutex_try_acquire above and
     * the atomic_or that sets WAITING (the first, or the PI republish), the
     * holder can release: its clear returns an owner word WITHOUT the waiting
     * bit, so it skips the wake — and a waiter that blocked at that moment
     * would sleep forever on a mutex whose owner slot is already free
     * (0x80000000: free + WAITING, which mutex_try_acquire treats as "not
     * free", so every later locker piles up behind the first one too).
     *
     * Re-read owner under the chain lock now that WAITING is published. If the
     * owner slot is free, the release landed in the window: take the mutex here
     * instead of blocking. Preserve WAITING iff other waiters remain queued
     * behind this one.
     */
    unsigned long ow = *(volatile unsigned long *) &sync->owner;
    if ((ow & RTMUX_SYNC_OWNER_MASK) == RTMUX_SYNC_MUTEX_FREE) {
        /* This waiter takes the mutex without ever blocking, so it bypasses the
         * scheduler wake barrier that the normal handoff path rides on. The
         * releaser published its unlock with a fence-then-store (see
         * __mutex_release_owner in cpucfg.h); pair it with an acquire fence
         * here so this thread observes everything the previous holder wrote
         * under the mutex before it touches the protected data.
         */
        cpu_smp_mb();
        thread_t *old_owner = pi_enabled ? sync_turnstile_owner_pin(ts) : NULL;

        sync_dequeue(c, act);
        unsigned long waiting = mutex_waiting_bit_locked(c, key, ts, NULL);

        *(volatile unsigned long *) &sync->owner = want | waiting;
        if (pi_enabled && waiting)
            sync_turnstile_set_owner_thread_locked(ts, act, want, 0);
        else if (pi_enabled) {
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
        }
        kspinlock_unlock(&c->lock);
        sync_pi_recompute_owner(old_owner);
        if (old_owner)
            tthread_unpin(old_owner);
        sync_ts_put(act, free_ts);
        mutex_lock_done(act, sync, recp, MUTEX_TAKEN);
        return;
    }

    /* A robust waiter gets the mutex with the releaser's record, so its own
     * goes to the spare slot or is freed. Do it now: once unready_release()
     * drops the chain lock, act may already own the mutex and be running on
     * another hart, and the caller must find nothing left to release.
     */
    free_rec = robust_rec_stash(act, *recp);
    *recp = NULL;

    /* Turnstile-style block. The chain lock is held here. unready_release()
     * stamps STATE_MUTEX, calls block_release() which picks a new active for
     * this CPU, updates pc_curthread, AND THEN drops c->lock. After that
     * release, a peer waker holding the chain (mutex_unlock) sees act on the
     * queue but pc_curthread != act on this CPU — ready_default's
     * dispatch-double-entry tripwire passes.
     */
    extern void unready_release(thread_t *, int, kspinlock_t *);
    unready_release(act, STATE_MUTEX, &c->lock);

    /* act may own the mutex and run on another hart by now, so its spare slot
     * is off limits: free the idle turnstile outright.
     */
    if (free_ts)
        _sfree(free_ts, sizeof(*free_ts));
    if (free_rec)
        _sfree(free_rec, sizeof(*free_rec));

    /* On wake, the unlocker has already transferred ownership to act by setting
     * sync->owner = SYNC_OWNER(act) AND removing act from the queue. No further
     * work needed here.
     */
}

/* Acquire a user mutex (kcall slot SYNC_MUTEX_LOCK).
 *
 * Tries the uncontested fast path first; on contention, queues the caller on
 * the mutex's chain and blocks turnstile-style. A lost-wakeup re-check handles
 * the holder releasing in user space between the fast-path read and the WAITING
 * publish. A robust mutex first gets a holder record ready, so that taking the
 * mutex never fails on an allocation. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the user sync_t to lock
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_mutex_lock(thread_t *act,
                               struct kcall_args_sync_mutex_lock *kap)
{
    sync_t *sync = kap->sync;
    struct robust_hold *rec = NULL;
    int rc;

    rc = sync_probe(act, sync);
    if (rc == EOK && mutex_robust(sync))
        rc = robust_rec_get(act, sync, &rec);
    if (rc != EOK) {
        kcall_err(act, rc);
        return ENOERROR;
    }
    mutex_lock_body(act, sync, &rec);
    robust_rec_put(act, rec);
    return ENOERROR;
}

/* Release a user mutex and wake the next waiter (kcall slot SYNC_MUTEX_UNLOCK).
 *
 * The user-side pthread_mutex_unlock has normally already cleared the owner
 * word and only entered the kernel because waiters were present; this kcall
 * dequeues and wakes the head waiter, handing ownership to it. The cv_wait body
 * also calls in directly, in which case ownership is released here. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the user sync_t to unlock
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_mutex_unlock(thread_t *act,
                                 struct kcall_args_sync_mutex_unlock *kap)
{
    sync_t *sync = kap->sync;
    int rc;

    rc = sync_probe(act, sync);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    /* User/kernel protocol contract for the sync_t lock word (sync_t lives in
     * user shared memory; the kernel touches it via the same VA the user does):
     *
     *   User-side pthread_mutex_unlock has already
     *     (a) verified ownership in user code,
     *     (b) decremented sync->count,
     *     (c) atomically cleared the owner-id bits of sync->owner, KEEPING
     *         RTMUX_SYNC_WAITING (a plain xchg to 0 would open a two-owner race
     *         with a fast-path locker; see __mutex_release_owner in cpucfg.h),
     *   and entered this kcall only because that release observed
     *   RTMUX_SYNC_WAITING — i.e. there are kernel-side waiters to wake.
     *
     * Therefore on the normal path, sync->owner here is the release sentinel
     * (RTMUX_SYNC_WAITING with no owner id), unless a waiter already
     * self-served in the futex re-check window above and left its own id in the
     * word. The kernel's job is to dequeue and wake the next waiter under the
     * chain lock. Ownership is re-read under that lock below rather than from a
     * racy pre-lock snapshot: it is the release sentinel, or -- on the cv_wait
     * body's direct release, which bypasses the user-side sequence -- still the
     * caller's own id, or already a self-served waiter's id.
     */
    unsigned long want = SYNC_OWNER(act);
    struct robust_hold *rec = NULL;

    sync_key_t key;
    if (sync_resolve_key(sync, &key) != 0) {
        kcall_err(act, EFAULT);
        return ENOERROR;
    }

    /* A robust mutex is released only by the holder the kernel recorded, and
     * one still inconsistent becomes unrecoverable (see ROBUST MUTEXES).
     */
    if (mutex_robust(sync)) {
        bool unrecoverable;

        rc = robust_release_begin(act, sync, key, &rec, &unrecoverable);
        if (rc != EOK) {
            kcall_err(act, rc);
            return ENOERROR;
        }
        if (unrecoverable) {
            mutex_make_unrecoverable(act, sync, key);
            return ENOERROR;
        }
    }

    struct sync_chain *c = sync_chain_of(key);
    kspinlock_lock(&c->lock);
    turnstile_t *ts = sync_turnstile_find_locked(c, key);
    int pi_enabled = mutex_pi_enabled(sync);
    thread_t *old_owner = sync_turnstile_owner_pin(ts);
    int ceiling_enabled =
        (((unsigned long) sync->count & RTMUX_SYNC_PRIOCEILING) != 0);

    /* Re-read the lock word under the chain lock. If it now names a real owner
     * that is not the releaser, a waiter already claimed the mutex through the
     * futex re-check window in ksys_sync_mutex_lock and -- if it left waiters
     * queued behind it -- now owns the duty of waking them on its own unlock.
     * The releaser has already surrendered ownership, so dequeuing and handing
     * off again here would grant the mutex to a second owner. Drop the
     * turnstile pin and return. (Unreachable for PRIO_PROTECT mutexes: their
     * unlock keeps owner == self until this kcall, so no waiter can self-serve
     * ahead of it.)
     */
    unsigned long cur = *(volatile unsigned long *) &sync->owner;
    unsigned long cur_owner = cur & RTMUX_SYNC_OWNER_MASK;
    if (cur_owner != RTMUX_SYNC_MUTEX_FREE && cur_owner != want) {
        kspinlock_unlock(&c->lock);
        sync_pi_recompute_owner(old_owner);
        if (old_owner)
            tthread_unpin(old_owner);
        robust_rec_put(act, rec);
        return ENOERROR;
    }

    /* Single owner transition under the chain lock. On the self-owned path,
     * folding the release into the handoff prevents observers from seeing a
     * transient free owner slot. A robust mutex files its record on the new
     * owner before the lock word names it; see robust_attach.
     */
    thread_t *next =
        rec ? robust_pick_owner(c, key, rec) : sync_dequeue_head(c, key);
    if (next) {
        unsigned long new_owner =
            SYNC_OWNER(next) | mutex_waiting_bit_locked(c, key, ts, NULL);
        *(volatile unsigned long *) &sync->owner = new_owner;
        if (pi_enabled && ts)
            sync_turnstile_set_owner_thread_locked(ts, next, new_owner, 0);
        rec = NULL;
        kspinlock_unlock(&c->lock);
        sync_pi_recompute_owner(old_owner);

        /* PRIO_PROTECT and PRIO_INHERIT are mutually exclusive. For a ceiling
         * mutex, drop the releaser's ceiling and raise the new owner to its
         * ceiling; mutex_prio_ceiling_boost already recomputes `next`, so do
         * not follow it with a PI recompute that would undo the ceiling. For PI
         * (or plain) mutexes, recompute the new owner from its turnstiles.
         */
        if (ceiling_enabled) {
            mutex_prio_ceiling_unboost(act, sync);
            mutex_prio_ceiling_boost(next, sync);
        } else {
            sync_pi_recompute_owner(next);
        }
        if (old_owner)
            tthread_unpin(old_owner);
        ready(next);
    } else {
        if (pi_enabled && ts) {
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
        }

        /* No waiter remains. The lock word is the release sentinel, a bare 0,
         * or (cv_wait direct release) the releaser itself -- the early return
         * above already peeled off any real other owner. Clear the sentinel (or
         * the releaser's id) to free; leaving the sentinel set would strand
         * every future locker (mutex_try_acquire treats 0x80000000 as
         * not-free). But do NOT rewrite a word that is already bare 0: a
         * concurrent fast-path CAS(0 -> self) -- which never takes this chain
         * lock -- may have just acquired the mutex (e.g. after a waiter cleared
         * WAITING on a PI-propagation failure), and a redundant store of 0
         * would clobber that new owner. When the sentinel or a real id is
         * present, no fast-path CAS can have won, so the store is safe.
         */
        if (cur != RTMUX_SYNC_MUTEX_FREE)
            *(volatile unsigned long *) &sync->owner = RTMUX_SYNC_MUTEX_FREE;
        kspinlock_unlock(&c->lock);
        sync_pi_recompute_owner(old_owner);
        if (ceiling_enabled)
            mutex_prio_ceiling_unboost(act, sync);
        if (old_owner)
            tthread_unpin(old_owner);
    }
    robust_rec_put(act, rec);
    return ENOERROR;
}

/* special-return.c calls these symbols directly during ACQUIRE_MUTEX. */
void sync_mutex_lock(thread_t *act, sync_t *sync, int flags)
{
    (void) flags;
    struct kcall_args_sync_mutex_lock kap = {.sync = sync};
    ksys_sync_mutex_lock(act, &kap);
}

/* CONDITION VARIABLE */

/* The reacquire on a condvar wake runs where nothing can be reported but the
 * lock's own result, so every allocation it may need is made before the mutex
 * is released, while an EAGAIN can still reach the caller with the mutex held:
 * an idle turnstile for a PI mutex, a holder record for a robust one. False
 * when out of memory.
 */
static bool condvar_reserve_reacquire(thread_t *act, sync_t *mutex)
{
    if (mutex_pi_enabled(mutex) && !act->ts_spare) {
        act->ts_spare = sync_ts_alloc(act);
        if (!act->ts_spare)
            return false;
    }
    if (mutex_robust(mutex) && !act->robust_spare) {
        act->robust_spare = _scalloc(sizeof(*act->robust_spare));
        if (!act->robust_spare)
            return false;
    }
    return true;
}

/* Wait on a condition variable, releasing the bound mutex atomically (kcall
 * slot SYNC_CONDVAR_WAIT).
 *
 * Holds the CV chain lock across both the cv-queue insert and the bound-mutex
 * release so a concurrent cv_signal cannot slip between them. On wake, the
 * ACQUIRE_MUTEX flag causes specialret to re-acquire the bound mutex before
 * returning to user mode. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the cv, kap->mutex the bound mutex
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_condvar_wait(thread_t *act,
                                 struct kcall_args_sync_condvar_wait *kap)
{
    sync_t *cv = kap->sync;
    sync_t *mutex = kap->mutex;
    int rc;

    rc = sync_probe(act, cv);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }
    rc = sync_probe(act, mutex);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    /* Caller must hold the bound mutex.  Verify ownership. */
    unsigned long want = SYNC_OWNER(act);
    if ((*(volatile unsigned long *) &mutex->owner & RTMUX_SYNC_OWNER_MASK) !=
        want) {
        kcall_err(act, EPERM);
        return ENOERROR;
    }

    /* POSIX atomicity requirement: release the mutex and register on the cv
     * queue atomically w.r.t. cv_signal.
     *
     * We hold BOTH the cv chain lock and the mutex chain lock across the
     * release-and-enqueue. Any concurrent cv_signal must take cv_c's lock to
     * see the new waiter; any concurrent mutex_lock fast-path failure goes
     * through mutex_c's lock to enqueue. Holding both locks closes both races
     * atomically.
     *
     * The two chains may collide (sync_chain_of hashes by user address, 128
     * buckets, ~1/128 collision for arbitrary pointers) or sort in arbitrary
     * order. Address-order acquisition prevents ABBA against peer cv_wait
     * callers; the same-chain case takes the single lock once because the
     * underlying kspinlock_t is NOT recursive.
     */
    sync_key_t cv_key, mutex_key;
    if (sync_resolve_key(cv, &cv_key) != 0 ||
        sync_resolve_key(mutex, &mutex_key) != 0) {
        kcall_err(act, EFAULT);
        return ENOERROR;
    }
    struct sync_chain *cv_c = sync_chain_of(cv_key);
    struct sync_chain *mutex_c = sync_chain_of(mutex_key);

    if (!condvar_reserve_reacquire(act, mutex)) {
        /* Not blocking: drop a timed wait's armed timeout, or the thread's next
         * condition wait would start with it.
         */
        act->timeout_flags = 0;
        kcall_err(act, EAGAIN);
        return ENOERROR;
    }

    /* A robust bound mutex is released here the way ksys_sync_mutex_unlock
     * releases one: only by its recorded holder, and an inconsistent one
     * becomes unrecoverable. The wait still goes through its release and
     * reacquire; the reacquire is what fails, with ENOTRECOVERABLE, and the
     * caller then holds no mutex.
     */
    struct robust_hold *mutex_rec = NULL;
    bool mutex_unrecoverable = false;
    if (mutex_robust(mutex)) {
        rc = robust_release_begin(act, mutex, mutex_key, &mutex_rec,
                                  &mutex_unrecoverable);
        if (rc != EOK) {
            act->timeout_flags = 0; /* not blocking: drop an armed timeout */
            kcall_err(act, rc);
            return ENOERROR;
        }
    }

    int same_chain = sync_lock_two(cv_c, mutex_c);

    /* Set ACQUIRE_MUTEX so specialret re-locks the bound mutex on wake.
     * args.mu.mutex carries the mutex pointer.
     *
     * Save the held recursion depth in args.mu.incr so specialret can restore
     * mutex->count on wake (mutex->count += incr). Zero only the depth for the
     * duration of the wait, KEEPING the type flags (NONRECURSIVE / NOERRORCHECK
     * / PRIONONE / PRIOINHERIT): the mutex is handed off to the next waiter
     * while act blocks, and that new owner must still see the mutex's real
     * type. Clearing all of count made count==0 look like a recursive,
     * no-errorcheck, PRIO_NONE mutex, so an ERRORCHECK/NORMAL re-lock by the
     * handoff owner silently bumped the count instead of returning EDEADLK, and
     * the dropped PRIOINHERIT bit skipped PI boosting. The restore stays
     * correct because count(flags) + incr(depth) == the original flags|depth. A
     * PRIO_PROTECT mutex keeps its depth in the low 16 bits (the ceiling sits
     * above them), so it must save the full depth too — a PRIO_PROTECT +
     * RECURSIVE mutex can be held at depth > 1, and clearing only one level
     * would hand the mutex off with a stale nonzero depth.
     */
    atomic_or(&act->flags, RTMUX_FLG_THR_ACQUIRE_MUTEX);
    act->args.mu.mutex = mutex;
    long saved_mutex_count = mutex->count;
    int bound_mutex_pi = (saved_mutex_count & RTMUX_SYNC_PRIOINHERIT) != 0;
    int bound_mutex_ceiling = (saved_mutex_count & RTMUX_SYNC_PRIOCEILING) != 0;
    unsigned long depth_mask = rtmux_sync_depth_mask(saved_mutex_count);
    act->args.mu.incr = (long) ((unsigned long) saved_mutex_count & depth_mask);
    mutex->count = (long) ((unsigned long) saved_mutex_count & ~depth_mask);

    sync_enqueue(cv_c, cv_key, act);

    /* Mutex release-handoff, inlined under both chain locks rather than
     * recursing through ksys_sync_mutex_unlock() (which would attempt to
     * acquire mutex_c again). Mirrors its keyed dequeue + atomic owner write.
     * Always self-owned: ownership was verified at entry and mutex_c has not
     * been dropped since.
     */
    turnstile_t *mutex_ts = sync_turnstile_find_locked(mutex_c, mutex_key);
    thread_t *old_mutex_owner = sync_turnstile_owner_pin(mutex_ts);

    /* An inconsistent mutex becomes unrecoverable here, under both chain locks
     * and after act is on the cv queue, so that no signal can slip between the
     * release and the wait; the drain leaves no waiter to hand it to.
     */
    if (mutex_unrecoverable) {
        mutex_store_unrecoverable(mutex);
        mutex_fail_waiters_locked(mutex_c, mutex_key);
    }
    thread_t *mutex_next =
        mutex_rec ? robust_pick_owner(mutex_c, mutex_key, mutex_rec)
                  : sync_dequeue_head(mutex_c, mutex_key);
    if (mutex_next) {
        unsigned long new_owner =
            SYNC_OWNER(mutex_next) |
            mutex_waiting_bit_locked(mutex_c, mutex_key, mutex_ts, NULL);
        *(volatile unsigned long *) &mutex->owner = new_owner;
        if (bound_mutex_pi && mutex_ts)
            sync_turnstile_set_owner_thread_locked(mutex_ts, mutex_next,
                                                   new_owner, 0);
        mutex_rec = NULL;
    } else {
        /* An unrecoverable mutex keeps its word; the reacquire on wake fails.
         */
        if (!mutex_unrecoverable)
            *(volatile unsigned long *) &mutex->owner = RTMUX_SYNC_MUTEX_FREE;
        if (bound_mutex_pi && mutex_ts) {
            sync_turnstile_set_owner_thread_locked(mutex_ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(mutex_c, mutex_ts);
        }
    }

    /* Drop the mutex chain iff it's a distinct lock instance. cv_c stays held;
     * unready_release drops it at the pc_curthread-update point.
     */
    if (!same_chain)
        kspinlock_unlock(&mutex_c->lock);

    sync_pi_recompute_owner_held(old_mutex_owner, cv_c);

    /* The recomputes below run under cv_c (dropped later by unready_release).
     * mutex_next/act are not queued on a foreign chain here, so their requeue
     * stays within cv_c and takes no out-of-order lock.
     */
    if (mutex_next) {
        if (bound_mutex_ceiling)
            sync_ceiling_push(mutex_next, mutex_prio_ceiling(mutex));
        sync_pi_recompute_owner_held(mutex_next, cv_c);
    }

    /* act is releasing the bound mutex to block on the cv: drop its ceiling
     * contribution. specialret re-acquires (and re-boosts) it on wake.
     */
    if (bound_mutex_ceiling) {
        sync_ceiling_pop(act, mutex_prio_ceiling(mutex));
        sync_pi_recompute_owner_held(act, cv_c);
    }
    if (old_mutex_owner)
        tthread_unpin(old_mutex_owner);
    robust_rec_put(act, mutex_rec); /* act's own slot: it is still running */

    if (mutex_next)
        ready(mutex_next);

    /* Turnstile-style block. unready_release stamps STATE_CONDVAR, dispatches
     * to a new active, and drops cv_c->lock at the precise moment pc_curthread
     * is updated. See block_release() in dispatch.c for the invariant.
     */
    extern void unready_release(thread_t *, int, kspinlock_t *);
    unready_release(act, STATE_CONDVAR, &cv_c->lock);

    /* On wake the ACQUIRE_MUTEX flag + args.mu.mutex causes specialret to
     * re-acquire "mutex" before returning to user mode.
     */
    return ENOERROR;
}

/* Wake one or all waiters on a condition variable (kcall slot
 * SYNC_CONDVAR_SIGNAL).
 *
 * Moves the chosen waiters off the cv queue under the chain lock, then readies
 * them after dropping the lock. Woken threads become mutex waiters via
 * specialret's ACQUIRE_MUTEX handling. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the cv, kap->all selects broadcast
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_condvar_signal(thread_t *act,
                                   struct kcall_args_sync_condvar_signal *kap)
{
    sync_t *cv = kap->sync;
    int all = kap->all;
    int rc;

    rc = sync_probe(act, cv);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    sync_key_t key;
    if (sync_resolve_key(cv, &key) != 0) {
        kcall_err(act, EFAULT);
        return ENOERROR;
    }
    struct sync_chain *c = sync_chain_of(key);
    kspinlock_lock(&c->lock);

    /* Move waiters off the queue. Walk to a local list, then ready them after
     * dropping the chain lock — ready() takes its own sched lock and may take
     * time.
     */
    thread_t *to_wake = NULL;
    do {
        thread_t *next = sync_dequeue_head(c, key);
        if (!next)
            break;

        /* Push onto local stack list via next.thread (next.thread is now NULL
         * after sync_dequeue, safe to repurpose).
         */
        next->next.thread = to_wake;
        to_wake = next;
    } while (all);

    kspinlock_unlock(&c->lock);

    /* Wake each. Each becomes a mutex waiter via specialret's ACQUIRE_MUTEX
     * handling. Leave state == STATE_CONDVAR; ready_default transitions it
     * itself (INVARIANT-3 forbids STATE_READY at entry).
     */
    while (to_wake) {
        thread_t *thp = to_wake;
        to_wake = thp->next.thread;
        thp->next.thread = NULL;
        ready(thp);
    }
    return ENOERROR;
}

/* SEMAPHORE */

/* Wait on (decrement) a semaphore (kcall slot SYNC_SEM_WAIT).
 *
 * Decrements the count atomically when positive; otherwise queues the caller on
 * the semaphore's chain and blocks (or returns EAGAIN when kap->try requests a
 * non-blocking try-wait). Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the semaphore, kap->try the try-flag
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_sem_wait(thread_t *act,
                             struct kcall_args_sync_sem_wait *kap)
{
    sync_t *sem = kap->sync;
    int try = kap->try;
    int rc;

    rc = sync_probe(act, sem);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    /* Decrement count atomically if positive — fast path. */
    long c;
    do {
        c = *(volatile long *) &sem->count;
        if (c <= 0)
            break;
    } while (_mux_smp_cmpxchg_ul((unsigned long *) &sem->count,
                                 (unsigned long) c,
                                 (unsigned long) (c - 1)) != (unsigned long) c);
    if (c > 0)
        return ENOERROR;

    if (try) {
        kcall_err(act, EAGAIN);
        return ENOERROR;
    }

    /* Slow path: enqueue, mark, block. */
    sync_key_t key;
    if (sync_resolve_key(sem, &key) != 0) {
        kcall_err(act, EFAULT);
        return ENOERROR;
    }
    struct sync_chain *ch = sync_chain_of(key);
    kspinlock_lock(&ch->lock);

    /* Re-check under the chain lock — a sem_post may have made count positive
     * between the fast-path probe and the chain acquire.
     */
    c = *(volatile long *) &sem->count;
    if (c > 0) {
        sem->count = c - 1;
        kspinlock_unlock(&ch->lock);
        return ENOERROR;
    }

    sync_enqueue(ch, key, act);

    /* Turnstile-style block.  Releases ch->lock at the dispatch swap. */
    extern void unready_release(thread_t *, int, kspinlock_t *);
    unready_release(act, STATE_SEM, &ch->lock);
    return ENOERROR;
}

/* Post to (increment) a semaphore (kcall slot SYNC_SEM_POST).
 *
 * If a waiter is queued, hands the post directly to it (count stays unchanged);
 * otherwise increments the count. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the semaphore
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_sem_post(thread_t *act,
                             struct kcall_args_sync_sem_post *kap)
{
    sync_t *sem = kap->sync;
    int rc;
    (void) act;

    rc = sync_probe(act, sem);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    sync_key_t key;
    if (sync_resolve_key(sem, &key) != 0) {
        kcall_err(act, EFAULT);
        return ENOERROR;
    }
    struct sync_chain *c = sync_chain_of(key);
    kspinlock_lock(&c->lock);

    thread_t *next = sync_dequeue_head(c, key);
    if (next) {
        /* Hand off the post directly — caller's count stays at 0; waker
         * decrements on the waiter's behalf by not bumping. Leave state ==
         * STATE_SEM; ready_default transitions it.
         */
        kspinlock_unlock(&c->lock);
        ready(next);
    } else {
        sem->count++;
        kspinlock_unlock(&c->lock);
    }
    return ENOERROR;
}

/* sync_create / sync_destroy / sync_ctl / sync_mutex_revive */

/* Initialize a user synchronization primitive (kcall slot SYNC_CREATE).
 *
 * Stamps the sync type into the user lock word so later operations can validate
 * the kind. No per-sync kernel object is allocated — the chains are static and
 * the user's sync_t storage is the lock word. Validates clock id (cond) and
 * protocol (mutex) from attr. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync, kap->attr, kap->type
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_create(thread_t *act, struct kcall_args_sync_create *kap)
{
    sync_t *sync = kap->sync;
    sync_attr_t *attr = kap->attr;
    unsigned type = kap->type;
    int rc;

    rc = sync_probe(act, sync);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    /* If attr was supplied, fault it in for read. */
    if (attr) {
        if (!WITHIN_BOUNDARY((uintptr_t) attr, (uintptr_t) (attr + 1),
                             act->process->boundary_addr)) {
            kcall_err(act, EFAULT);
            return ENOERROR;
        }
    }

    /* Initialize the user lock word. The hash-chain design has no per-sync
     * kernel object to allocate — the user's sync_t storage IS the lock word,
     * and chains are static. This kcall is now largely bookkeeping: stamp the
     * sync type into owner/count so later ops can validate the kind.
     */
    switch (type) {
    case RTMUX_SYNC_COND: {
        int clockid = attr ? attr->clockid : CLOCK_REALTIME;
        switch (clockid) {
        case CLOCK_REALTIME:
        case CLOCK_MONOTONIC:
        case CLOCK_SOFTTIME:
            break;
        default:
            kcall_err(act, EINVAL);
            return ENOERROR;
        }
        sync->owner = RTMUX_SYNC_COND;
        sync->count = clockid;
        break;
    }
    case RTMUX_SYNC_SEM:
        sync->owner = RTMUX_SYNC_SEM;
        /* sync->count was set by the caller to the initial value */
        break;
    case RTMUX_SYNC_MUTEX_FREE:
        sync->count = RTMUX_SYNC_PRIONONE;
        if (attr) {
            int protocol = attr->protocol;

            if (protocol == PTHREAD_PRIO_INHERIT &&
                (attr->flags & RTMUX_ATTR_MUTEX) == 0) {
                protocol = PTHREAD_PRIO_NONE;
            }
            switch (protocol) {
            case PTHREAD_PRIO_INHERIT:
                /* Explicit pthread mutex attributes opt into the turnstile
                 * path. Zero-filled attrs are normalized to PRIO_NONE above
                 * because PTHREAD_PRIO_INHERIT is encoded as zero.
                 */
                sync->count = RTMUX_SYNC_PRIOINHERIT;
                break;
            case PTHREAD_PRIO_NONE:
                sync->count = RTMUX_SYNC_PRIONONE;
                break;
            case PTHREAD_PRIO_PROTECT:
                /* The ceiling occupies bits 16-23, so it must fit in 8 bits
                 * (priorities are 1-255); the disjoint low 16 bits carry the
                 * recursion depth.
                 */
                if (attr->prioceiling <= 0 ||
                    (unsigned long) attr->prioceiling >
                        (RTMUX_SYNC_CEIL_MASK >> RTMUX_SYNC_CEIL_SHIFT)) {
                    kcall_err(act, EINVAL);
                    return ENOERROR;
                }

                /* Seed the ceiling (depth 0) and default to non-recursive;
                 * pthread_mutex_init() clears NONRECURSIVE for a RECURSIVE
                 * type, so a PRIO_PROTECT mutex now honors its type like any
                 * other.
                 */
                sync->count = RTMUX_SYNC_NONRECURSIVE | RTMUX_SYNC_PRIOCEILING |
                              ((unsigned long) attr->prioceiling
                               << RTMUX_SYNC_CEIL_SHIFT);
                break;
            default:
                kcall_err(act, EINVAL);
                return ENOERROR;
            }
        }
        sync->owner = RTMUX_SYNC_MUTEX_FREE;
        break;
    default:
        kcall_err(act, EINVAL);
        return ENOERROR;
    }
    return ENOERROR;
}

/* Mark a user synchronization primitive as destroyed (kcall slot SYNC_DESTROY).
 *
 * Stamps RTMUX_SYNC_DESTROYED into the lock word; a well-behaved program drains
 * its waiters before calling this, so the queue is not walked here. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the primitive to destroy
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_destroy(thread_t *act, struct kcall_args_sync_destroy *kap)
{
    sync_t *sync = kap->sync;
    int rc;

    rc = sync_probe(act, sync);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    /* Mark as destroyed. Any thread waiting on this primitive will see
     * RTMUX_SYNC_DESTROYED on its next operation and return EINVAL. The kcall
     * does NOT walk the queue to wake waiters — a well-behaved program drains
     * its waiters before destroy.
     */
    sync->owner = RTMUX_SYNC_DESTROYED;
    sync->count = 0;
    {
        sync_key_t key;

        /* Best-effort turnstile cleanup: if the key no longer resolves, the
         * object's page is already gone, so there is nothing to free.
         */
        if (sync_resolve_key(sync, &key) == 0) {
            struct sync_chain *c = sync_chain_of(key);
            turnstile_t *ts;

            kspinlock_lock(&c->lock);
            ts = sync_turnstile_find_locked(c, key);
            sync_turnstile_free_idle_locked(c, ts);
            kspinlock_unlock(&c->lock);
        }
    }
    return ENOERROR;
}

/* Synchronization control operations (kcall slot SYNC_CTL).
 *
 * Priority-ceiling get/set is not yet implemented; reports ENOSYS. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (unused)
 * Return value: ENOERROR (ENOSYS is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_ctl(thread_t *act, struct kcall_args_sync_ctl *kap)
{
    /* TODO: SYNC_CTL_SETCEILING, SYNC_CTL_GETCEILING etc. For now return ENOSYS
     * so callers know it's unimplemented.
     */
    (void) kap;
    kcall_err(act, ENOSYS);
    return ENOERROR;
}

/* Revive an orphaned (robust) mutex (kcall slot SYNC_MUTEX_REVIVE).
 *
 * The caller takes ownership of a mutex whose previous owner died. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the mutex to revive
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_mutex_revive(thread_t *act,
                                 struct kcall_args_sync_mutex_revive *kap)
{
    sync_t *sync = kap->sync;
    int rc;

    rc = sync_probe(act, sync);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    /* Mutex-revive: caller takes ownership of a mutex without any record of it.
     * A robust mutex must be taken through the lock path instead, which records
     * the holder and reports EOWNERDEAD (see ROBUST MUTEXES).
     */
    if (mutex_robust(sync)) {
        kcall_err(act, EINVAL);
        return ENOERROR;
    }
    if (sync->owner == RTMUX_SYNC_DEAD ||
        sync->owner == RTMUX_SYNC_MUTEX_FREE ||
        sync->owner == RTMUX_SYNC_INITIALIZER) {
        sync->owner = SYNC_OWNER(act);
        sync->count = 0;
        return ENOERROR;
    }
    kcall_err(act, EBUSY);
    return ENOERROR;
}
