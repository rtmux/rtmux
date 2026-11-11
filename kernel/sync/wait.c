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
 * validation and boost/deboost.
 *
 * * STUBBED (return ENOERROR / EINVAL as appropriate; safe but functionally
 * incomplete): sync_ctl, sync_mutex_revive (mutex ownership-revival).
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
 * died without releasing, and the effect is limited to a misplaced PI boost; the
 * full pid cannot be recovered from the 32-bit owner word without widening it.
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
        if (thp && thp->tid == tid && tthread_pin(thp) != EOK)
            thp = NULL;
    }
    rcu_read_unlock(&tthread_rcu);
    rcu_read_unlock(&process_rcu);
    return thp;
}

/* Caller holds sync_pi_slock. Effective priority floor = max of the thread's
 * base priority, every PI turnstile it owns, and its PRIO_PROTECT ceiling
 * floor. Recompute is therefore idempotent: it never deboosts a thread below a
 * ceiling it still holds.
 */
static unsigned sync_pi_target_locked(thread_t *owner)
{
    unsigned target = owner->real_priority;

    for (turnstile_t *ts = owner->owned_turnstiles; ts; ts = ts->owner_next) {
        if (target < ts->max_waiter_prio)
            target = ts->max_waiter_prio;
    }
    if (owner->held_ceiling_n != 0 && target < owner->held_ceiling_floor)
        target = owner->held_ceiling_floor;
    return target;
}

static void sync_pi_recompute_owner_held(thread_t *owner,
                                         struct sync_chain *held)
{
    unsigned target;

    if (!owner)
        return;

    kspinlock_lock(&sync_pi_slock);
    target = sync_pi_target_locked(owner);
    kspinlock_unlock(&sync_pi_slock);

    if (owner->priority != target) {
        _TRACE_SYS_EMIT_PI_BOOST(owner, owner->priority, target);
        adjust_priority(owner, target, owner->dpp, 1);
        sync_chain_requeue_waiter_locked(owner, held);
    }
}

static void sync_pi_recompute_owner(thread_t *owner)
{
    sync_pi_recompute_owner_held(owner, NULL);
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

static void sync_turnstile_bind_waiter(turnstile_t *ts, thread_t *thp)
{
    kspinlock_lock(&sync_pi_slock);
    thp->blocked_on_turnstile = ts;
    kspinlock_unlock(&sync_pi_slock);
    ts->contenders++;
}

static void sync_turnstile_update_max_locked(struct sync_chain *c,
                                             turnstile_t *ts)
{
    unsigned max_prio = 0;

    for (thread_t *cur = c->head; cur; cur = cur->next.thread) {
        if (cur->args.mu.wait_key == ts->key && max_prio < cur->priority)
            max_prio = cur->priority;
    }

    kspinlock_lock(&sync_pi_slock);
    ts->max_waiter_prio = max_prio;
    kspinlock_unlock(&sync_pi_slock);
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
    turnstile_t *visited[SYNC_PI_MAX_DEPTH];
    turnstile_t *ts = start_ts;
    unsigned target;
    unsigned depth = 0;
    int result = EOK;

    sync_turnstile_ref(ts);

    while (ts) {
        thread_t *owner;
        turnstile_t *next_ts;

        if (depth == SYNC_PI_MAX_DEPTH) {
            sync_turnstile_unref_deferred(ts);
            result = EDEADLK;
            goto out;
        }
        for (unsigned i = 0; i < depth; i++) {
            if (visited[i] == ts) {
                sync_turnstile_unref_deferred(ts);
                result = EDEADLK;
                goto out;
            }
        }
        visited[depth++] = ts;

        owner = sync_turnstile_owner_pin(ts);
        if (!owner) {
            result = EOK;
            goto out;
        }
        if (owner == waiter) {
            tthread_unpin(owner);
            result = EDEADLK;
            goto out;
        }

        kspinlock_lock(&sync_pi_slock);
        target = sync_pi_target_locked(owner);
        if (target < waiter->priority)
            target = waiter->priority;
        kspinlock_unlock(&sync_pi_slock);

        if (owner->priority != target) {
            _TRACE_SYS_EMIT_PI_BOOST(owner, owner->priority, target);
            adjust_priority(owner, target, owner->dpp, 1);
            sync_chain_requeue_waiter_locked(owner, NULL);
        }
        next_ts = sync_thread_blocked_ts_ref(owner);
        tthread_unpin(owner);

        ts = next_ts;
    }

out:
    for (unsigned i = 0; i < depth; i++) {
        sync_turnstile_unref_deferred(visited[i]);
    }
    return result;
}

void rdecl sync_thread_cleanup(thread_t *thp)
{
    for (unsigned i = 0; i < SYNC_NCHAINS; i++) {
        struct sync_chain *c = &sync_chains[i];

        kspinlock_lock(&c->lock);
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

/* Wait queue primitives (LINK2 on thread_t next.thread/prev.thread). */

/* Caller holds chain lock. */
static void sync_enqueue(struct sync_chain *c, sync_key_t key, thread_t *thp)
{
    thread_t *cur = c->head;
    thread_t **insertp = &c->head;

    thp->args.mu.wait_key = key;
    thp->blocked_on = NULL;

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
    if (thp->args.mu.wait_key == key && thp->prev.thread) {
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
 * paths (timeout, signal/cancel, destroy). force_ready() only understands
 * ksync_t wait lists; chain waiters identify their queue by args.mu.wait_key.
 */
int rdecl sync_chain_force_ready(thread_t *thp)
{
    sync_key_t key = thp->args.mu.wait_key;
    struct sync_chain *c;

    if (!key)
        return 0;

    c = sync_chain_of(key);
    kspinlock_lock(&c->lock);
    if (thp->args.mu.wait_key == key && thp->prev.thread) {
        turnstile_t *ts;
        thread_t *owner;

        kspinlock_lock(&sync_pi_slock);
        ts = thp->blocked_on_turnstile;
        kspinlock_unlock(&sync_pi_slock);
        owner = sync_turnstile_owner_pin(ts);
        sync_dequeue(c, thp);
        if (thp->state == STATE_MUTEX && !sync_has_waiter(c, key)) {
            if (ts) {
                sync_turnstile_free_idle_locked(c, ts);
            }
        }
        kspinlock_unlock(&c->lock);
        sync_pi_recompute_owner(owner);
        if (owner)
            tthread_unpin(owner);
        return 1;
    }
    kspinlock_unlock(&c->lock);
    return 1;
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

/* Try to acquire the mutex atomically.
 *
 * Returns 1 on success, 0 if already owned by someone else, -1 if the owner
 * field indicates a non-mutex sync type.
 */
static int mutex_try_acquire(thread_t *act, sync_t *sync)
{
    unsigned long want = SYNC_OWNER(act);
    unsigned long owner = *(volatile unsigned long *) &sync->owner;

    if (owner == RTMUX_SYNC_INITIALIZER || owner == RTMUX_SYNC_MUTEX_FREE) {
        if (_mux_smp_cmpxchg_ul(&sync->owner, owner, want) == owner) {
            return 1;
        }
        return 0; /* race lost, fall through to slow path */
    }
    /* Owner non-zero: check for non-mutex types */
    if (owner == RTMUX_SYNC_DESTROYED)
        return -1;
    if ((owner & ~RTMUX_SYNC_WAITING) == want) {
        if ((sync->count & RTMUX_SYNC_NONRECURSIVE) != 0)
            return -2;
        /* Recursive acquire by same owner — count the recursion. */
        sync->count++;
        return 1;
    }
    return 0;
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

/* Acquire a user mutex (kcall slot SYNC_MUTEX_LOCK).
 *
 * Tries the uncontested fast path first; on contention, queues the caller on
 * the mutex's chain and blocks turnstile-style. A lost-wakeup re-check handles
 * the holder releasing in user space between the fast-path read and the WAITING
 * publish. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args; kap->sync is the user sync_t to lock
 * Return value: ENOERROR (any error is reported to the caller via kcall_err).
 */
int kdecl ksys_sync_mutex_lock(thread_t *act,
                               struct kcall_args_sync_mutex_lock *kap)
{
    sync_t *sync = kap->sync;
    unsigned long want = SYNC_OWNER(act);
    turnstile_t *allocated_ts = NULL;
    turnstile_t *free_ts = NULL;
    int waiter_bound = 0;
    int pi_enabled;
    int rc;

    rc = sync_probe(act, sync);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }
    rc = mutex_prio_ceiling_check(act, sync);
    if (rc) {
        kcall_err(act, rc);
        return ENOERROR;
    }

    /* Uncontested fast path */
    rc = mutex_try_acquire(act, sync);
    if (rc == 1) {
        mutex_prio_ceiling_boost(act, sync);
        return ENOERROR;
    }
    if (rc == -1) {
        kcall_err(act, EINVAL);
        return ENOERROR;
    }
    if (rc == -2) {
        kcall_err(act, EDEADLK);
        return ENOERROR;
    }

    /* Contested: resolve the physical key (before any chain lock), then queue
     * ourselves under the chain lock and block.
     */
    sync_key_t key;
    if (sync_resolve_key(sync, &key) != 0) {
        kcall_err(act, EFAULT);
        return ENOERROR;
    }
    struct sync_chain *c = sync_chain_of(key);
retry_contended:
    kspinlock_lock(&c->lock);

    /* Double-check: the holder may have released between the fast-path read and
     * the chain-lock acquire. Try once more under the lock.
     */
    rc = mutex_try_acquire(act, sync);
    if (rc == 1) {
        kspinlock_unlock(&c->lock);
        if (allocated_ts)
            _sfree(allocated_ts, sizeof(*allocated_ts));
        mutex_prio_ceiling_boost(act, sync);
        return ENOERROR;
    }
    if (rc == -2) {
        kspinlock_unlock(&c->lock);
        if (allocated_ts)
            _sfree(allocated_ts, sizeof(*allocated_ts));
        kcall_err(act, EDEADLK);
        return ENOERROR;
    }

    /* Mark the lock word as having waiters. This is observed by mutex_unlock to
     * know it must wake someone.
     */
    atomic_or_64((volatile unsigned long *) &sync->owner, RTMUX_SYNC_WAITING);
    pi_enabled = mutex_pi_enabled(sync);
    turnstile_t *ts = NULL;
    if (pi_enabled) {
        ts = sync_turnstile_find_locked(c, key);
        if (!ts) {
            if (!allocated_ts) {
                kspinlock_unlock(&c->lock);
                allocated_ts = _scalloc(sizeof(*allocated_ts));
                if (!allocated_ts) {
                    kcall_err(act, EAGAIN);
                    return ENOERROR;
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
    if (pi_enabled)
        sync_turnstile_record_owner(ts,
                                    *(volatile unsigned long *) &sync->owner);
    if (pi_enabled) {
        sync_turnstile_bind_waiter(ts, act);
        waiter_bound = 1;
        kspinlock_unlock(&c->lock);
        rc = sync_pi_propagate(act, ts);
        kspinlock_lock(&c->lock);
        if (rc != EOK) {
            thread_t *owner = sync_turnstile_owner_pin(ts);

            if (waiter_bound) {
                sync_turnstile_unbind_waiter_locked(c, act);
                waiter_bound = 0;
            }
            if (!sync_has_waiter(c, key)) {
                unsigned long owner_word =
                    *(volatile unsigned long *) &sync->owner;
                *(volatile unsigned long *) &sync->owner =
                    owner_word & ~RTMUX_SYNC_WAITING;
                sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
                sync_turnstile_free_idle_locked(c, ts);
            }
            kspinlock_unlock(&c->lock);
            sync_pi_recompute_owner(owner);
            if (owner)
                tthread_unpin(owner);
            kcall_err(act, rc);
            if (free_ts)
                _sfree(free_ts, sizeof(*free_ts));
            return ENOERROR;
        }

        rc = mutex_try_acquire(act, sync);
        if (rc == 1) {
            if (waiter_bound) {
                sync_turnstile_unbind_waiter_locked(c, act);
                waiter_bound = 0;
            }
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
            kspinlock_unlock(&c->lock);
            if (free_ts)
                _sfree(free_ts, sizeof(*free_ts));
            mutex_prio_ceiling_boost(act, sync);
            return ENOERROR;
        }
        if (rc == -1) {
            if (waiter_bound) {
                sync_turnstile_unbind_waiter_locked(c, act);
                waiter_bound = 0;
            }
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
            kspinlock_unlock(&c->lock);
            kcall_err(act, EINVAL);
            if (free_ts)
                _sfree(free_ts, sizeof(*free_ts));
            return ENOERROR;
        }
        if (rc == -2) {
            if (waiter_bound) {
                sync_turnstile_unbind_waiter_locked(c, act);
                waiter_bound = 0;
            }
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
            kspinlock_unlock(&c->lock);
            kcall_err(act, EDEADLK);
            if (free_ts)
                _sfree(free_ts, sizeof(*free_ts));
            return ENOERROR;
        }
        sync_turnstile_record_owner(ts,
                                    *(volatile unsigned long *) &sync->owner);
    }

    /* Enqueue self, transition state under the chain lock. */
    sync_enqueue(c, key, act);
    if (pi_enabled)
        sync_turnstile_update_max_locked(c, ts);

    /* Lost-wakeup guard (futex re-check). The holder releases the mutex
     * entirely in user space: pthread_mutex_unlock clears the owner-id bits of
     * the lock word (keeping RTMUX_SYNC_WAITING; see __mutex_release_owner in
     * cpucfg.h) and only enters ksys_sync_mutex_unlock when that release
     * observed RTMUX_SYNC_WAITING. The chain lock does NOT serialize that
     * user-side release. So between the under-lock mutex_try_acquire above and
     * the atomic_or that sets WAITING, the holder can release: its clear
     * returns an owner word WITHOUT the waiting bit, so it skips the wake — and
     * a waiter that blocked at that moment would sleep forever on a mutex whose
     * owner slot is already free (0x80000000: free + WAITING, which
     * mutex_try_acquire treats as "not free", so every later locker piles up
     * behind the first one too).
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
        unsigned long no = want;
        if (sync_has_waiter(c, key))
            no |= RTMUX_SYNC_WAITING;
        *(volatile unsigned long *) &sync->owner = no;
        if (pi_enabled && sync_has_waiter(c, key))
            sync_turnstile_set_owner_thread_locked(ts, act, want, 0);
        else if (pi_enabled) {
            sync_turnstile_set_owner_thread_locked(ts, NULL, 0, 0);
            sync_turnstile_free_idle_locked(c, ts);
        }
        kspinlock_unlock(&c->lock);
        sync_pi_recompute_owner(old_owner);
        if (old_owner)
            tthread_unpin(old_owner);
        if (free_ts)
            _sfree(free_ts, sizeof(*free_ts));
        mutex_prio_ceiling_boost(act, sync);
        return ENOERROR;
    }

    /* Turnstile-style block. The chain lock is held here. unready_release()
     * stamps STATE_MUTEX, calls block_release() which picks a new active for
     * this CPU, updates pc_curthread, AND THEN drops c->lock. After that
     * release, a peer waker holding the chain (mutex_unlock) sees act on the
     * queue but pc_curthread != act on this CPU — ready_default's
     * dispatch-double-entry tripwire passes.
     */
    extern void unready_release(thread_t *, int, kspinlock_t *);
    unready_release(act, STATE_MUTEX, &c->lock);
    if (free_ts)
        _sfree(free_ts, sizeof(*free_ts));

    /* On wake, the unlocker has already transferred ownership to act by setting
     * sync->owner = SYNC_OWNER(act) AND removing act from the queue. No further
     * work needed here.
     */
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

    sync_key_t key;
    if (sync_resolve_key(sync, &key) != 0) {
        kcall_err(act, EFAULT);
        return ENOERROR;
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
        return ENOERROR;
    }

    /* Single owner transition under the chain lock. On the self-owned path,
     * folding the release into the handoff prevents observers from seeing a
     * transient free owner slot.
     */
    thread_t *next = sync_dequeue_head(c, key);
    if (next) {
        unsigned long new_owner = SYNC_OWNER(next);
        if (sync_has_waiter(c, key))
            new_owner |= RTMUX_SYNC_WAITING;
        *(volatile unsigned long *) &sync->owner = new_owner;
        if (pi_enabled && ts)
            sync_turnstile_set_owner_thread_locked(ts, next, new_owner, 0);
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
    unsigned long depth_mask =
        bound_mutex_ceiling ? RTMUX_SYNC_PPDEPTHMASK : RTMUX_SYNC_COUNTMASK;
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

    thread_t *mutex_next = sync_dequeue_head(mutex_c, mutex_key);
    if (mutex_next) {
        unsigned long new_owner = SYNC_OWNER(mutex_next);
        if (sync_has_waiter(mutex_c, mutex_key))
            new_owner |= RTMUX_SYNC_WAITING;
        *(volatile unsigned long *) &mutex->owner = new_owner;
        if (bound_mutex_pi && mutex_ts)
            sync_turnstile_set_owner_thread_locked(mutex_ts, mutex_next,
                                                   new_owner, 0);
    } else {
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

    /* Mutex-revive: caller takes ownership of an "orphaned" mutex whose
     * previous owner died. The kcall just claims it; full EOWNERDEAD semantics
     * is a TODO.
     */
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
