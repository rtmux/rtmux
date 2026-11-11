# RTMux Mutex PI/PC Design

This is the design gate for `PTHREAD_PRIO_INHERIT` and
`PTHREAD_PRIO_PROTECT`.  It must be reviewed before kernel code advertises
realtime POSIX mutex protocol compliance.

## Current State

`kernel/sync/wait.c` uses FreeBSD umtx-style hashed wait queues keyed by the
user `sync_t *`.  Contended `PTHREAD_PRIO_INHERIT` mutexes now allocate a
per-key turnstile, record the observed owner, propagate bounded inherited
priority through nested wait chains, and return `EDEADLK` on cycle/depth-cap
detection.  `PTHREAD_PRIO_PROTECT` mutexes store their ceiling in
`sync_t.count` while unlocked, enter through the kernel lock path, reject
callers above the ceiling, and boost/deboost the owner while held.

## Turnstile Object

Allocate a `turnstile_t` lazily on first contention for a sync key.

Required fields:

- sync key: user `sync_t *` plus owning process/address-space identity;
- chain linkage under the existing sync-chain hash lock;
- owner `thread_t *`, pinned while the chain lock is held or by an explicit
  reference;
- priority-ordered waiter queue for this key only;
- owner-held list linkage so deboost can examine all turnstiles boosting an
  owner;
- protocol and ceiling state for priority protection;
- flags for dying owner, destroyed mutex, timeout/cancel removal, and handoff.

Do not resurrect one permanent `ksync_t` per primitive.  The object exists only
while contention or protocol state requires kernel memory.

The waiter queue is per key, never per hash chain.  A chain collision must only
share the chain lock; it must not share waiters, owners, or priority ordering.

## Locking

The sync-chain lock remains `LOCK_CLASS_TURNSTILE` and stays below
`LOCK_CLASS_SCHED`.  The turnstile wait queue is protected by the chain lock
that owns its key.  If two sync keys are involved, acquire their chains in
address order.  Scheduler mutations still happen through the existing block and
ready paths.

No PI path may dereference an owner thread after leaving the RCU read section
unless the chain lock or a thread reference keeps the object alive.

`sync_lock_two(a, b)` is the only allowed helper for condvar paths that touch a
CV key and a mutex key together.  It locks the two owning chains in address
order and returns a flag for the same-chain case.  `CONFIG_LOCK_DEBUG` rejects
ad hoc distinct-instance `LOCK_CLASS_TURNSTILE` nesting unless this helper
authorized the pair.

Lazy allocation uses lookup/drop/allocate/retry:

1. Lock the chain and look for the turnstile.
2. If missing, drop the chain lock.
3. Allocate from normal kernel memory or a future per-CPU turnstile cache.
4. Re-lock the chain and re-check; if another thread won, free or cache the new
   object.

No allocator is called while a sync-chain spinlock is held.

## Priority Inheritance

On contended lock:

1. Re-check the user lock word under the chain lock.
2. Find or allocate the turnstile for the key.
3. Insert the waiter by effective priority.
4. Record the waiter's `blocked-on-turnstile`.
5. Boost the owner if the waiter outranks it.
6. Propagate the boost if the owner is itself blocked on another turnstile.
7. Block through the scheduler with the chain lock released at the existing
   handoff point.

Owner identity is copied into the per-key turnstile from the observed owner
word.  Boost code must not reread the user `sync_t` owner word while holding a
chain spinlock.  Before boosting, the identity is resolved through the thread
lookup path and revalidated under `tthread_rcu`; `tthread_pin()` keeps the
pointer usable after `rcu_read_unlock()`.  If cleanup has cleared a turnstile owner
for an already-seen owner word, later contention must not re-pin a recycled
thread with that same raw `(pid, tid)` word.

Boost propagation walks:

`waiter -> blocked_on_turnstile -> owner -> blocked_on_turnstile`

The walk uses a visited stamp and a hard depth cap.  Default cap: 16.  On a
cycle or cap hit, stop propagation, emit a trace/log record, and return
`EDEADLK` to the topmost contender.  Unbounded propagation under a spinlock is
a realtime bug.

On unlock, timeout, signal/cancel, force-ready, owner death, or mutex destroy:

- remove the waiter or transfer ownership under the chain lock;
- recompute the old owner's effective priority from its remaining owned
  turnstiles;
- deboost before returning to user mode;
- wake the highest-priority eligible waiter, not FIFO head.

## Priority Ceiling

`PTHREAD_PRIO_PROTECT` needs ceiling state visible to both libc and the kernel.
RTMux stores the unlocked ceiling in `sync_t.count` as
`RTMUX_SYNC_NONRECURSIVE | RTMUX_SYNC_PRIOCEILING | ceiling`.  PRIO_PROTECT
mutexes deliberately bypass the lockless libc fast path so the kernel can
validate the caller priority and apply the temporary ceiling boost even when the
mutex is uncontended.  Kernel-only ceiling state would miss direct user-space
inspection, while a lockless fast path would miss boost/deboost.

Lock attempt rules:

- fail with the POSIX error when the caller's priority exceeds the ceiling;
- boost the owner to the ceiling while it holds the mutex;
- restore the previous effective priority on unlock after all inherited boosts
  are recomputed;
- reject recursive PRIO_PROTECT construction in this ABI shape; the ceiling
  shares `sync_t.count` low bits with the lock depth, so recursive ceiling
  mutexes need a future metadata expansion before they can be represented
  without ambiguity.

## Exit Paths

Every path below must deboost or transfer ownership correctly:

| Exit cause | Function(s) to update | Required action |
| --- | --- | --- |
| normal mutex unlock | `ksys_sync_mutex_unlock`, `sync_mutex_unlock` slow path | transfer owner or deboost after remaining owned turnstiles are recomputed |
| timed wait timeout | timeout path, `force_ready` | remove waiter, recompute owner boost from remaining waiters |
| signal/cancel force-ready | `force_ready`, `cancel_unblock` | same as timeout; never blindly clear owner boost |
| thread exit while blocked | thread teardown | unlink from blocked turnstile, recompute old owner |
| process death while owning | `process_term`, thread teardown | mark owner dying, wake or error waiters, clear held-turnstile list |
| mutex destroy with waiters | `mutex_destroy` path | mark destroyed, wake waiters with error, remove boosts |
| priority change | `adjust_priority_default`, `__sched_set` | recompute effective priority from base plus all owned turnstiles |
| condvar reacquire | `ksys_sync_condvar_wait` re-lock leg | move from CV wait to mutex turnstile without collision-mixing |

When a non-winning waiter leaves the queue, recompute the owner's boost from
remaining waiters.  Clearing the boost simply because one waiter left is wrong
when another higher-than-base waiter remains.

Implemented PI paths remove chain-keyed waiters with
`sync_chain_force_ready()`, avoid asynchronous user-memory dereferences during
timeout cleanup, recompute owner boosts from remaining turnstiles, and free
idle turnstiles after the last contender/owner/reference leaves.  The table
remains the audit map for future priority-ceiling work.

Legacy fields in `kernel/include/objects.h` are not reused blindly.  The design
replacement is:

- `thread_t.blocked_on_turnstile`: explicit pointer for PI propagation;
- `thread_t.owned_turnstiles`: owner-held list used for deboost and death;
- `ksync_t.mutex_holdlist`: retired from new PI code unless a replacement proves
  it has the exact lifetime and granularity above.

## RCU And Pinning

`tthread_pin(thread_t *)` and `tthread_unpin(thread_t *)` are the required
promotion API before PI code keeps an owner pointer beyond `rcu_read_unlock()`.
Callers may pin only a pointer already protected by `tthread_rcu` or a stronger
subsystem lock.  Thread teardown removes the thread from the process vector,
drains `tthread_rcu`, then waits for `pin_count` to reach zero before recycling
the object, so pre-existing readers can either drop the pointer or publish an
explicit pin.  A chain lock alone is a pin only while the owner field is still
protected by that same chain lock and the thread is still linked from a
turnstile reachable through the chain.

Turnstile lifetime must outlive every contender and every boost-propagation
walk that can observe it.  Acceptable implementations are either SMR-reclaimed
turnstiles or a contender refcount.  Plain free on last waiter is not valid
until all owner-held-list and blocked-on references are gone.

## Test Plan

- libc protocol API tests for `pthread_mutexattr_setprotocol()`, ceiling
  setters/getters, PRIO_PROTECT lock boost/deboost, and ceiling-violation
  rejection;
- direct `Sync*` tests for timeout, destroy, and contender cleanup paths;
- `tests/userland/rt-latency/pi-inversion-xfail.c`, now a required-pass PI
  probe;
- two-mutex cycle regression that returns `EDEADLK`;
- hash-collision regression proving key A unlock cannot wake key B's waiter;
- TID-reuse regression proving a timed-out contender cannot become the mutex
  owner through a recycled TID;
- SMP stress with `CONFIG_LOCK_DEBUG` enabled.

## Section 13 SMT refinement table

The N-hart SRR + bounded-PI safety invariants are machine-checked at the spec
level by `tests/smt/srr-pi-safety.smt2` (`make check-srr-pi-smt`).  Each solver
assertion maps to the concrete function that must uphold it, its linearization
point, and the runtime probe that covers the lock-free C the model does not:

| Invariant (SMT assertion) | Concrete function / linearization point | Covering probe |
| --- | --- | --- |
| No priority leak: effective priority returns to base once the last waiter leaves | `adjust_priority()` deboost recompute from the owner's remaining owned turnstiles on the unlock / timeout / cancel / owner-death path (`kernel/sched/dispatch.c`); linearizes at the effective-priority recompute under the chain lock when the waiter set empties | `rt-check` PI rows in `docs/rt-budgets.md`; the `pi-inversion` test |
| A boost never lowers the owner below its waiter | `sync_pi_propagate()` `target = max(sync_pi_target_locked(owner), waiter->priority)` (`kernel/sync/wait.c:576-584`); linearizes at the `adjust_priority` under `sync_pi_slock` | `pi-inversion` / `pi-inversion-xfail`; `rt-check` |
| Bounded PI walk: a cycle or over-deep chain returns `EDEADLK` (no unbounded spinlock work) | `sync_pi_propagate()` visited-stamp + `SYNC_PI_MAX_DEPTH` cap + self-cycle check (`kernel/sync/wait.c:551-570`); linearizes at the revisit / cap / self test at each iteration head | `CONFIG_LOCK_DEBUG` bounded chain-lock hold; `rt-check` |
| No thread runs on two harts | the locked ready-list unlink in `select_thread_default()` / `dispatch_unlink_ready()` (`kernel/sched/dispatch.c:206,257`; `runcpu`/`STATE_RUNNING` are published afterward, not the claim); linearizes at the ready-node unlink under the dispatch lock | `CONFIG_LOCK_DEBUG` under SMP; the `thread_destroy` "stuck active on hart" FATAL guard; reference-host `CPUS=8` `rt-check` |
| No transition acts on a stale (recycled `pid`,`tid`) receiver | `sync_turnstile_owner_pin()` process-vector resolve + `tid` recheck + `tthread_pin()`, plus the stale-owner guard that refuses an already-cleared owner word (`kernel/sync/wait.c:336-339,421`); linearizes at the `tid` recheck and pin before first use | the RCU litmus (`tests/litmus/rcu-sched-qs-publish.litmus`); the `blocked_on`/`rcvid` UAF regression; `rt-check` |

The SMT proves only that the invariants are self-consistent as stated; the
linked C is validated by the named probes, per the section 13 gate philosophy
(the model excludes the concurrent turnstile list, run queues, and RCU epochs).

## IPC Message-Pass Priority Donation

Separately from mutex PI above, synchronous IPC donates the client's priority to
the receiving server thread for the duration of a request, so a high-priority
client is not head-of-line blocked behind the server's low base priority. Two
paths apply the donation:

- Rendezvous (a receiver is already waiting): the receiver thread runs at the
  sender's message-driven priority and partition, assigned before it returns to
  user mode (kernel/ipc/fastmsg.c, the receiver-found branch sets
  thp->real_priority = thp->priority = sender->priority and inherits
  sender->dpp).
- Send-block (no receiver waiting): the server threads that last used the channel
  are raised to the sender's priority if they are below it, so the message is
  serviced at the client's priority rather than the server's base.

Supported donation boundary (verified by tests/userland/rt-latency/
ipc-priority-donation.c, in the rt-check gate; it exercises the rendezvous and
transitive paths, not the send-block scan):

- Propagation: a low-priority server observes its own sched_curpriority equal to
  the high-priority client's priority while processing the request.
- No stale priority leak: a second request from the same client at a lower
  priority is serviced at that lower priority, not the earlier higher one, so a
  donation does not survive into the next unrelated request. The receive path
  overwrites the effective priority with each new sender; the test checks this
  per-request tracking, not that the server thread is separately observed back at
  its base between requests.
- Transitivity: a server that is itself blocked on a second server (client to A
  to B, A sending to B while reply-blocked on it) passes the donation through, so
  B also observes the client's priority.

The donation stays within fixed-priority scheduling: the rendezvous path runs the
receiver at the sender's message-driven priority, the send-block scan only raises
a server thread that is below the sender (never lowers it), and the effective
priority does not persist into an unrelated later request. It cannot preempt a
server that is already running a handler on the client's behalf; that
availability bound, not a donation gap, is what the LKL client path hits below.

## Scope: LKL Client Path Excluded

This PI/PC model covers native RTMux mutexes and the threads that contend on
them.  It does not extend to the LKL client request path.  RTMux IPC already
boosts each LKL server's single dispatch worker to the waiting client's priority
(the dispatch channel is not fixed-priority), so ordinary receive priority
inheritance applies.  The residual problem is not a missing PI hook but the
single synchronous worker: it runs one request — including any blocking LKL
syscall — to completion before receiving the next, so a high-priority client is
head-of-line blocked behind an in-flight low-priority request, and no kernel PI
mechanism can preempt a user-space server mid-handler.  That is an availability/
latency bound, not a mutex-protocol gap.  The LKL client path is therefore
declared non-realtime; see `docs/lkl-domain.md` "Realtime Scope" for the
worst-case head-of-line characterization and the deferred multi-worker option.
