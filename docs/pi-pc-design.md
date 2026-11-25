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
| priority change | `sched_thread` (`__sched_set`), `sched_ss_update`/`sched_ss_adjust` | set the base only (`thread_set_base_prio`), then reapply `max(base, loans)`; see "Base and Effective Priority" |
| condvar reacquire | `ksys_sync_condvar_wait` re-lock leg | move from CV wait to mutex turnstile without collision-mixing |

Each owner keeps its turnstiles in a priority list keyed by their highest
waiter. Recomputing the owner's inherited priority reads the first entry.
Updating a loan skips equal-priority groups, so insertion visits at most
`NUM_PRI` priority levels. Debug list validation can also walk the entries.
The `--pi-owner-loans` guest test releases three contended mutexes out of
priority order and checks the effective priority after each handoff.

Worst case under `sync_pi_slock`, interrupts off: a loan update is one
`pril_rem` and one `pril_add` on the owner's list. Each steps once per distinct
priority level on that list, and turnstile loans carry no notification boost
bit, so a list has at most `NUM_PRI` = 256 levels and an update takes at most
512 list steps however many mutexes the owner holds. Reading the owner's
inherited priority (`sync_pi_target_locked`) takes constant time.

Thread exit keeps the rule that whichever of the dying owner and a contender
comes second sees the other, but pays for it only when there is something to
see. `thread_t.sync_owner_flags` has a TRACKED bit, set (with an atomic OR)
whenever the kernel records the thread as an owner: a contender naming it in a
lock word, a turnstile taking it as owner, a robust record filed on it, or the
thread itself queueing on a mutex it may be handed. Exit sets CLOSED with the
same atomic OR. One of the two ORs comes first: if TRACKED was already set,
exit scans the 128 chains as before; if CLOSED came first, the registration
sees it and treats the thread as a dead owner, and exit returns without
taking a chain lock. The bit is sticky until the thread is recycled, so a
thread that ever contended a kernel-visible mutex pays the scan at exit.

`thread_t.blocked_on_turnstile` is written only under the chain lock of the
key its thread queues under, and cleared under `sync_pi_slock` as well. A
holder of that chain lock may therefore read it without the PI lock; a reader
that holds only `sync_pi_slock` may read it and take a turnstile reference.
Waiters bound to no turnstile (semaphores, condition variables, mutexes
without `PRIO_INHERIT`) leave their chain without touching `sync_pi_slock`.

When a non-winning waiter leaves the queue, recompute the owner's boost from
remaining waiters.  Clearing the boost simply because one waiter left is wrong
when another higher-than-base waiter remains.

Implemented PI paths remove chain-keyed waiters with
`sync_chain_force_ready()`, avoid asynchronous user-memory dereferences during
timeout cleanup, recompute owner boosts from remaining turnstiles, and free
idle turnstiles after the last contender/owner/reference leaves.  The table
remains the audit map for future priority-ceiling work.

A blocked PI waiter records its turnstile in `thread_t.blocked_on_turnstile`.
Each owner links its turnstiles through `thread_t.owned_turnstiles` for
priority recomputation and thread teardown.

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

## Base and Effective Priority

Every thread has a base priority, `thread_t.real_priority`, that only its own
scheduling parameters change: `SchedSet` (`sched_thread`), and the sporadic
server's drop to its low priority and return to its normal one
(`sched_ss_update`, `sched_ss_adjust`). None of these is a loan. Other threads
lend it priority through three fields beside it:

- `pi_loan`: the highest waiter on any PI turnstile it owns, and its held
  PRIO_PROTECT ceiling floor. It is the cached result of
  `sync_pi_target_locked()`, refreshed under `sync_pi_slock` whenever an owned
  turnstile's waiters or the ceiling set change. Ceilings are therefore part of
  this model, not a separate path.
- `msg_loan`: the request (a client REPLY-blocked on this thread) or the
  notification it is serving. It packs the priority, a request flag and a
  generation that every new loan bumps (`thread_lend_msg`).
- `pend_loan`: the highest send or notification still queued on the channel it
  serves. A queued send raises it on the channel's server threads; a receive
  or reply resets it to the queue head, storing it under `chp->lock` so that a
  send queued meanwhile raises it again afterwards.

The IPC paths write the two message fields with plain atomics: no lock and no
allocation on the fast path.

The scheduler runs the thread at its effective priority, `max(base, pi_loan,
msg_loan, pend_loan)`, cached in `thread_t.priority`. A loan is a floor: a
server serving a lower-priority client runs at its own base. Writes to these
fields go only through `kernel/include/thread-prio.h` (`thread_set_base_prio`,
`thread_lend_msg`, `thread_relend_msg`, `thread_set_pend_loan`,
`thread_raise_pend_loan`, `thread_init_prio`; `pi_loan` only in
`kernel/sync/wait.c`), enforced by `make check-prio-writes`.

`prio_apply()` turns the inputs into `thread_t.priority` through
`adjust_priority()`, under `sync_pi_slock` (ranked below the dispatch, channel
and sync locks `adjust_priority` takes). Every writer of an input calls it
after its store, so the last one to take the lock reads every store made
before it; the loop re-reads after each adjust until the priority matches. An
unlocked comparison, after a fence, skips the lock when nothing changes. The
rendezvous fast path stores the effective priority directly into a receiver
that is on no queue and then calls `thread_prio_settle()`, which is that same
comparison.

A thread created by a server while it serves a request starts at the
request's priority, so a process spawned through roottask inherits its
spawner's priority as POSIX requires; otherwise a new thread starts at its
creator's base (`thread_inherit_prio`). PI loans are never inherited. A
creator that blocks until the new thread first runs (`pthread_create`) lends it
its own running priority, loans included, until then, so it never waits on a
thread below itself; the new thread's base is still the inherited priority.

### When a message loan starts and ends

| Event | Loan |
| --- | --- |
| receive of a request (rendezvous, or from the send queue) | `msg_loan` = sender's effective priority, new generation, request flag; `pend_loan` = head of the send queue after it |
| `MsgCurrent` naming a client (`msg_current_take`) | `msg_loan` = that client's effective priority, new generation, request flag; on a `RTMUX_FLG_CHF_FIXED_PRIORITY` channel it lends nothing, so `msg_loan` is 0 with no request flag and an empty loan is left as it was; `pend_loan` unchanged |
| receive of a notification, or a remote (`COF_NETCON`) message | `msg_loan` = its priority, new generation, no request flag, until the next receive |
| receive on a `RTMUX_FLG_CHF_FIXED_PRIORITY` channel | `msg_loan` = 0 |
| `thread_create` whose creator blocks until the new thread runs, with the creator running above the priority the new thread inherits | the new thread's `msg_loan` = the creator's running priority, no request flag; `thread_specret` ends it once it has readied the creator and applied any explicit policy, or a `SchedSet` on the new thread ends it before that |
| send or notification queued with no receiver | `pend_loan` raised to its priority on each thread whose `last_chid` is the channel, whatever that thread's current priority |
| `MsgReply` / `MsgError` that leaves the thread with no client | the request loan ends; if the call completed an exchange on the channel the thread serves, `pend_loan` = the send-queue head read as the client was unlinked |
| reply to an older client while serving a newer one | unchanged: the newer client's loan stays |
| the client leaves REPLY by timeout, signal, cancellation or death | `force_ready` clears the server's `client` link and ends the request loan of that client (`thread_client_departed`, the same primitive `MsgCurrent` uses) and queues the server on the hart's deferred-settle list (`thread_prio_defer_settle`); it takes no lock and walks nothing, since it runs under caller locks and in interrupt context. The hart's next kernel exit then recomputes the server's priority and follows the chain it is blocked in, before the hart resumes or idles, so a second server or mutex owner is lowered too, whether or not the server runs. A client that leaves between the server's receive taking it and installing its loan is caught by the receive (`thread_end_orphan_loan`). `MsgCurrent` ends the old client's loan on its error returns as well. The server's later `MsgReply` or `MsgError` fails with `ESRCH` |
| `MsgCurrent(0)` | the request loan ends at once; a notification loan stays until the next receive |
| `SchedSet` on the thread, whether or not it changes the priority or policy | a notification loan ends, as the priority a receive set was replaced before loans were separate; `pend_loan` is read again, under the channel lock, from the queue of the channel the thread last received on (0 when that channel is gone), so a send another thread has since taken no longer holds it; a request loan and `pi_loan` stay |
| server thread dies | its client's `args.ms.server` is cleared before `tthread_rcu` drains; nothing is lent any more |

Until one of those ends it, a stale loan can only hold a server above its base
(over-boost): no path lowers a thread below a loan that is still live.

### REPLY propagation

A priority change carries along the wait-for chain in `prio_chain_walk()`
(`kernel/sync/wait.c`). From a thread whose effective priority changed:

- if it waits on a PI turnstile, the owner is recomputed (as before, with the
  waiter's priority folded into the owner's `pi_loan`);
- if it is REPLY-blocked, its server's request loan is set to its new effective
  priority, and the walk continues from that server, through further REPLY,
  SEND or mutex waits;
- if it is SEND-blocked, the `pend_loan` of the channel's server threads is
  raised to the send-queue head, and the walk ends there.

So a priority-60 waiter on a mutex whose owner is REPLY-blocked on a
priority-10 server runs that server at 60, and the loan follows changes in both
directions: when the waiter times out or is cancelled, the owner's recompute
walks the same chain and lowers the server again (`prio-loan` case 2).

The server is found under the lock of the channel the client waits on
(`prio_ipc_hop_find`): with the client's state and `blocked_on` rechecked there,
`args.ms` is the live member of the reused `args` union, and the server must
still name the client as its `client`. The server's loan generation is read
before that check, and the update is a compare-and-swap on it
(`thread_relend_msg`): a receive sets the new client before bumping the
generation, so a walk that saw the old client can never overwrite the new
client's loan.

Bounds and policies:

- Depth: at most `CONFIG_PRIO_REPLY_DEPTH` IPC hops per walk (Kconfig, default
  8, range 1..64; `prio.reply_depth` in `/sys/kstats`), plus at most
  `SYNC_PI_MAX_DEPTH` turnstiles. Past the IPC bound the walk stops: the
  threads beyond it keep the priority they had (they are not boosted, and a
  later change that fits the bound still reaches them), and
  `prio.reply_depth_overflow` counts the stop. No error is returned: the thread
  whose change started the walk is not deadlocked. `prio-loan` case 5 builds a
  chain one longer than the bound and checks both.
- Cycles: an explicit mutex lock that encounters a mutex-only cycle returns `EDEADLK`. An IPC hop that comes
  back to the contending waiter (it holds the request its mutex's owner waits
  on) is also a cycle and returns `EDEADLK` to that waiter instead of blocking
  forever (`prio-loan` case 4). The failed lock then walks the owner's chain
  again unconditionally (`sync_pi_recompute_chain`), taking the contender's
  floor back out of every thread it reached. A condition waiter has already
  released its mutex before its reacquire encounters the cycle. The walk stops
  at the same bound, and the waiter stays queued for handoff. The
  `--cond-pi-cycle` fixture times out another participant and verifies that
  the condition waiter returns holding its mutex.
- Re-sorting: every adjust re-sorts the thread in the queue it waits on: the
  dispatch queue, a channel send queue (`adjust_priority_default`), or its
  sync chain (`sync_chain_requeue_waiter_locked`, which first finds the thread
  on that chain under the chain lock, since `wait_key` and the links alias other
  state once it has left).
- Simultaneous loans: a thread holding a PI mutex and serving a message runs
  at the larger of the two, and losing one leaves the other (`prio-loan`
  case 1).
- Concurrent walks: two walks lending for the same client retry their
  compare-and-swap until the one that reads the client's latest priority
  stores it; a walk that finds a newer generation leaves it.
- Not followed: the condvar wait leg, which holds a chain lock, recomputes the
  two owners it touches without walking on; the sporadic drop of a thread
  already blocked under the dispatch lock (`SS_STOP_RUNNING_LOCKED`) stores
  its effective priority directly, and the next loan or base change reapplies
  it (the other sporadic drop and the replenishment go through
  `thread_prio_update_dpp`); a SEND-blocked sender's lower priority does not
  lower `pend_loan`, which is reset at the next receive or reply.
- Known window, unchanged from before: a contender bound to a turnstile but not
  yet queued on it contributes its priority only through its own walk's
  floor, so a concurrent recompute of the owner in that window can drop it
  until the contender is queued and the turnstile maximum is next refreshed.

## Section 13 SMT refinement table

The N-hart SRR + bounded-PI safety invariants are machine-checked at the spec
level by `tests/smt/srr-pi-safety.smt2` (`make check-srr-pi-smt`).  Each solver
assertion maps to the concrete function that must uphold it, its linearization
point, and the runtime probe that covers the lock-free C the model does not:

| Invariant (SMT assertion) | Concrete function / linearization point | Covering probe |
| --- | --- | --- |
| No priority leak: effective priority returns to base once the last waiter leaves | `sync_pi_recompute_owner()` refreshes `pi_loan` from the owner's remaining owned turnstiles (`sync_pi_target_locked`) and `prio_apply()` reapplies `max(base, loans)` on the unlock / timeout / cancel / owner-death path (`kernel/sync/wait.c`); linearizes at the `pi_loan` store under `sync_pi_slock` when the waiter set empties | `rt-check` PI rows in `docs/rt-budgets.md`; the `pi-inversion` test; `prio-loan` cases 1 and 2 |
| A boost never lowers the owner below its waiter | `prio_chain_walk()` turnstile step: `sync_pi_target_locked(owner, floor)` with `floor = waiter->priority` folds the waiter into `pi_loan` (`kernel/sync/wait.c`); linearizes at that store under `sync_pi_slock` | `pi-inversion` / `pi-inversion-xfail`; `rt-check` |
| Bounded PI walk: a cycle or over-deep chain returns `EDEADLK` (no unbounded spinlock work) | `prio_chain_walk()` visited-stamp + `SYNC_PI_MAX_DEPTH` cap + self-cycle check, the last also on an IPC hop back to the waiter (`kernel/sync/wait.c`); linearizes at the revisit / cap / self test at each iteration head | `CONFIG_LOCK_DEBUG` bounded chain-lock hold; `rt-check`; `prio-loan` case 4 |
| No thread runs on two harts | the locked ready-list unlink in `select_thread_default()` / `dispatch_unlink_ready()` (`kernel/sched/dispatch.c:206,257`; `runcpu`/`STATE_RUNNING` are published afterward, not the claim); linearizes at the ready-node unlink under the dispatch lock | `CONFIG_LOCK_DEBUG` under SMP; the `thread_destroy` "stuck active on hart" FATAL guard; reference-host `CPUS=8` `rt-check` |
| No transition acts on a stale (recycled `pid`,`tid`) receiver | `sync_lookup_owner_pin()` process-vector resolve + `tid` recheck + `tthread_pin()`, plus the stale-owner guard that refuses an already-cleared owner word (`kernel/sync/wait.c`); linearizes at the `tid` recheck and pin before first use | the RCU litmus (`tests/litmus/rcu-sched-qs-publish.litmus`); the `blocked_on`/`rcvid` UAF regression; `rt-check` |
| Base and loans are separate: a base change keeps every live loan, and the base returns once the loans end | `thread_effective_prio()` = `max(real_priority, pi_loan, msg_loan, pend_loan)` (`kernel/include/thread-prio.h`); `sched_thread()` and the sporadic drop/return write only `thread_set_base_prio()`; linearizes at `prio_apply()`'s adjust after the base or loan store | `prio-loan` cases 1, 3 and 6; `make check-prio-writes` |
| Bounded REPLY propagation: at most `CONFIG_PRIO_REPLY_DEPTH` hops, and a longer chain is counted | `prio_chain_walk()` REPLY step `++hops > CONFIG_PRIO_REPLY_DEPTH` stop + `prio_reply_depth_overflow` (`kernel/sync/wait.c`); linearizes at the hop test | `prio-loan` case 5 (`/sys/kstats` `prio.reply_depth`, `prio.reply_depth_overflow`) |

Invariants 6 and 7 (the last two rows) model the base/loan split and the REPLY
hop bound.

The SMT proves only that the invariants are self-consistent as stated; the
linked C is validated by the named probes, per the section 13 gate philosophy
(the model excludes the concurrent turnstile list, run queues, and RCU epochs).

## IPC Message-Pass Priority Donation

Separately from mutex PI above, synchronous IPC donates the client's priority to
the receiving server thread for the duration of a request, so a high-priority
client is not head-of-line blocked behind the server's low base priority. Two
paths apply the donation:

- Rendezvous (a receiver is already waiting): the sender lends its priority to
  the receiver for the request (kernel/ipc/fastmsg.c, the receiver-found branch
  lends `sender->priority` as the receiver's request loan and runs it at its
  effective priority, inheriting sender->dpp). The receiver's base is untouched.
- Send-block (no receiver waiting): the server threads that last used the channel
  raise their `pend_loan` to the sender's priority, so the message is serviced
  at the client's priority rather than the server's base.

Supported donation boundary (verified by tests/userland/rt-latency/
ipc-priority-donation.c, in the rt-check gate; it exercises the rendezvous and
transitive paths, not the send-block scan):

- Propagation: a low-priority server observes its own sched_curpriority equal to
  the high-priority client's priority while processing the request.
- No stale priority leak: a second request from the same client at a lower
  priority is serviced at that lower priority, not the earlier higher one, so a
  donation does not survive into the next unrelated request. The receive path
  overwrites the effective priority with each new sender; the test checks this
  per-request tracking; `prio-loan` (syscall-testing) checks that a server is
  back at its base after the reply.
- Transitivity: a server that is itself blocked on a second server (client to A
  to B, A sending to B while reply-blocked on it) passes the donation through, so
  B also observes the client's priority.

The donation stays within fixed-priority scheduling: the rendezvous path lends
the sender's priority, the send-block scan only raises a server thread that is
below the sender (never lowers it), and the loan ends with the reply, so it does
not persist into an unrelated later request. A loan is a floor, never a cap: a
server whose base is above its client's priority serves it at its base. It cannot preempt a
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
