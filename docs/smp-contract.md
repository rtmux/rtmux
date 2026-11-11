# RTMux SMP Contract

This document is the cross-file contract for BKL-less RTMux kernel code.  Local
comments remain the authority for implementation detail; this file states the
rules new code must satisfy before it participates in SMP scheduling, IPC,
interrupt, or sync paths.

## Lock Classes

Lock ranks are defined by `enum lock_class` in `kernel/include/lock.h`.
Numerically lower classes are acquired first.  Nested acquisition must move
from lower rank to higher rank unless the local comment documents an
instance-ordering rule.

| Rank | Class | Scope | Notes |
| --- | --- | --- | --- |
| 1 | `LOCK_CLASS_TURNSTILE` | sync-chain wait queues | Per-key chains in `kernel/sync/wait.c`; may hand off to scheduler blocking paths. |
| 2 | `LOCK_CLASS_TURNSTILE_OWNER` | owner-held PI turnstile lists | Acquired after a sync-chain lock; dropped before scheduler priority adjustments. |
| 3 | `LOCK_CLASS_SCHED` | per-CPU dispatch | `kdispatch_t` is the per-CPU runqueue state.  Multiple scheduler locks are ordered by stable dispatch address / hart id. |
| 4 | `LOCK_CLASS_TIMER` | timer queues | Timer queue mutation only. |
| 5 | `LOCK_CLASS_INTREVENT` | interrupt event queues | IRQ attach/detach and event delivery metadata. |
| 6 | `LOCK_CLASS_PROCVEC` | process vectors | Lookup namespace before channel/connect object locks. |
| 7 | `LOCK_CLASS_CHANNEL` | channel object | Message receive queues and channel scalar state. |
| 8 | `LOCK_CLASS_CONNECT` | connection object | Send-side connection state. |
| 9 | `LOCK_CLASS_SYNC` | `ksync_t` paths | Do not use for new sync-chain code. |
| 10 | `LOCK_CLASS_PROCESS` | process scalar state | Not for vector mutation. |
| 11 | `LOCK_CLASS_THREAD` | thread scalar state | Per-thread fields outside queue ownership. |
| 12 | `LOCK_CLASS_MEMPART` | kernel heap | May call into physical allocator. |
| 13 | `LOCK_CLASS_PA` | physical allocator | Leaf after memory partition. |
| 14 | `LOCK_CLASS_DEBUG` | debugger/crash interlocks | Avoid acquiring normal subsystem locks while held. |

`CONFIG_LOCK_DEBUG` records nested acquisitions and rejects rank cycles.  It is
part of the realtime test surface, not optional documentation.  It only catches
an inversion on a schedule that actually runs; `scripts/check-lock-rank.py`
(`make check-lock-rank`, in the default `make check`) complements it by reading
these ranks from `enum lock_class` and statically flagging any nested acquire in
`kernel/` that moves from a higher rank down to a lower one, whether or not that
path executes.  It covers `kspinlock_lock` sites and the `SCHED_LOCK*` wrapper
macros; same-class distinct-instance nesting (the `sync_lock_two` turnstile
pair, dual `vec_slock`, per-hart dispatch locks) is reported on an allowlist
(`-v`), not failed, matching the runtime rule.  It is intra-procedural and
flow-insensitive, so it under-reports (an inversion split across a conditional
unlock is left to runtime `LOCK_DEBUG`) but never invents a false inversion; an
acquire whose class cannot be resolved fails the gate rather than passing
silently.

## Module Lock Map

| Module | Locks taken | Rank order / graph |
| --- | --- | --- |
| `kernel/include/lock.h` | declares `kspinlock_t` and `LOCK_CLASS_*`; takes no locks itself | Defines the global rank graph.  New classes must be inserted with an explicit lower-to-higher nesting reason. |
| `kernel/debug/lock-debug.c` | per-CPU held-lock stack; may use crash/debug paths | Debug state is per-CPU while recording normal locks.  On violation it enters crash/debug handling and must not acquire ordinary subsystem locks after that point. |
| `kernel/include/rcu.h` / `kernel/sync/rcu.c` | RCU-sched epoch plus per-CPU quiescent-state slots; no spinlock in readers | RCU read sections are outside the spinlock rank graph, but they must not block or call scheduler paths.  Writers may wait for readers only after unpublishing pointers. |
| `kernel/sync/wait.c` | sync-chain `LOCK_CLASS_TURNSTILE`; PI owner list `LOCK_CLASS_TURNSTILE_OWNER`; scheduler handoff through block/ready paths | `TURNSTILE -> TURNSTILE_OWNER`; priority recompute drops PI state before taking `SCHED`.  No allocation while the chain lock is held.  Two sync keys must use `sync_lock_two()` for address-ordered chain locking; `CONFIG_LOCK_DEBUG` rejects ad hoc turnstile pairs. |
| `kernel/sched/dispatch.c` | dispatch `LOCK_CLASS_SCHED` | Local runqueue lock only for same-CPU operations; multiple dispatch locks by ascending hart id / stable address. |
| `kernel/time/timer-core.c` | timer object `LOCK_CLASS_TIMER`; scheduler locks for wakeup | `TIMER -> SCHED` only through documented wake paths; do not hold timer locks across unbounded callback work. |
| `kernel/irq/interrupt.c` and `arch/riscv/trap-handler.c` | interrupt metadata `LOCK_CLASS_INTREVENT`; scheduler wake path | `INTREVENT -> SCHED` for readying attached ISTs; hard-IRQ code does not allocate or block. |
| `kernel/ipc/channel.c` | process vector `LOCK_CLASS_PROCVEC`; channel `LOCK_CLASS_CHANNEL`; connect `LOCK_CLASS_CONNECT` | `PROCVEC -> CHANNEL -> CONNECT`; drop object locks before scheduler block/ready. |
| `kernel/ipc/message.c` | channel/connect locks plus scheduler locks | Queue mutation under `CHANNEL`/`CONNECT`; state transition through scheduler helpers after queue membership is published. |
| `kernel/thread/lifecycle.c` | process vector `LOCK_CLASS_PROCVEC`; thread scalar `LOCK_CLASS_THREAD`; scheduler locks | `PROCVEC -> THREAD -> SCHED` only for bounded local state changes; teardown does not walk arbitrary foreign queues. |
| `kernel/object/core.c` and allocators | memory partition `LOCK_CLASS_MEMPART`; physical allocator `LOCK_CLASS_PA` | `MEMPART -> PA`; allocator locks are leaf locks and not held across IPC/scheduler state changes. |
| `kernel/debug/*` | debug/crash `LOCK_CLASS_DEBUG` | Crash/debug locks are terminal.  Avoid acquiring ordinary subsystem locks once held. |

## RCU Lifetime Rules

RTMux exposes hot lookup lifetime rules through `kernel/include/rcu.h`.  The
current backend is RCU-sched: read-side sections are compiler barriers, and
writers wait for every other live hart to leave its current kernel call or
publish a later quiescent state.  Quiescent states are recorded at context
switch, U-mode return, dead-thread exit, and idle entry.
`tests/litmus/rcu-sched-qs-publish.litmus` is the RVWMO proof obligation for
turning that accounting into reclamation: a reader's protected load must not be
observed after a writer has seen the reader's quiescent-state store.
On reference-host, run the litmus gate after `eval $(opam env --switch=rtmux-herd)`;
that switch provides the known-good herdtools7/herd7 7.58 toolchain.

Readers:

- Open the read section before loading a pointer from the protected vector.
- Do not block, sleep, call `block()`, or otherwise release the CPU inside the
  section.
- Treat the pointer as valid only until `rcu_read_unlock()`.

Writers:

- Unpublish the pointer first.
- Do not hold a `kspinlock_t` across `rcu_sched_synchronize()`.
- Do not call `rcu_sched_synchronize()` from a hard-RT deadline path.  RCU readers are
  wait-free compiler barriers, but grace-period waits spin until peer harts leave
  their current kernel call or publish a quiescent state.
- Call `rcu_sched_synchronize()` only from teardown, revoke, or other non-deadline
  paths until an embedded-node `call_rcu` queue exists.
- Recycle storage only after the grace period completes.

Hard-RT use:

- RCU read sections are allowed in hard-RT lookup paths because they do not take
  locks, issue atomic RMW operations, write shared state, or block.
- A grace-period wait is bounded only by the longest non-preemptible kernel call
  on any live hart plus scheduling/TCG noise in QEMU.  It is not a hard-RT
  operation.
- Future async reclamation must use objects that embed their callback node and
  run the blocking grace-period wait/free work on non-RT utility harts.  Do not
  add allocation-backed `rcu_head` nodes to kernel hot paths.  A public
  `call_rcu(struct rcu_head *, void (*func)(struct rcu_head *))` API should
  wait until the first object-specific embedded-node user proves the lifetime
  and drain rules.
- For `thread_t` reclamation, the first async form should be object-specific:
  enqueue only after the joiner has been readied, use a `thread_t`-embedded
  SPSC node, and let exactly one background utility worker on a non-RT hart
  perform lookup removal via `vector_rem()`.  Keep the owning `process_t`
  pinned until `vector_rem()`, the RCU grace-period wait,
  `tthread_wait_unpinned(thp)`, the no-`pc_curthread` confirmation,
  `object_free(prp, &thread_souls, thp)`, and any last-thread notification are
  complete.  Opportunistic same-hart or remote-idle draining is not the first
  handoff policy; previous measurements regressed the post-store rows.  Any
  worker still needs explicit queue-drain bounds plus the same RCU, pin, and
  active-thread gates.
- Do not create that worker through the normal `thread_create()` path unless that
  path grows an explicit kernel-worker mode.  Today `thread_create()` prepares
  non-loading processes for U-mode entry and WAAA startup; an RCU reaper must stay
  an S-mode kernel worker.
- A hand-built S-mode reaper trial was rejected before the benchmark
  began: local/reference-host build gates passed, but CPUS=8 QEMU crashed in
  `unready_release()` from `thread_reaper_wait()` with `pc_curthread` NULL on
  CPU 7.  A future worker must first establish the kernel-worker dispatch
  invariant that the running worker is also the hart's `pc_curthread` before it
  sleeps through `unready_release()` or any other scheduler blocking primitive.
- A narrower worker-only trial removed the reclaim queue and crashed even
  earlier: the S-mode worker reached its entry point on CPU 7, but
  `get_active_thread(get_cpunum())` was already NULL.  The required contract is
  therefore a kernel-worker bootstrap/dispatch contract, not only a queue
  wakeup contract.
- `kernel_thread_create()` is the allowed bootstrap for future S-mode utility
  workers.  It creates a kernel-process thread, keeps `RTMUX_FLG_THR_WAAA`
  clear, initializes an S-mode saved context, and queues through `ready()` so
  the existing scheduler/idle path installs `pc_curthread` before entry.
  Reaper prototypes must use this constructor plus `unready_release_and_exit()`
  for worker sleep before moving any RCU-protected reclamation tail.
- The first constructor-backed post-ready reclaim worker was rejected: CPUS=8
  `rcu-lookup-measure` reached the benchmark marker, then crashed in lifecycle
  teardown.  A future handoff needs a proof of exclusive ownership of the
  flagged `prp->threads[tid]` slot after `ready(joiner)` before a worker calls
  `vector_rem()`.
- A narrower worker that left `vector_rem()` inline and deferred only the
  post-removal RCU/free tail was also rejected: the worker itself trapped at
  `sepc=0` in the lifecycle benchmark.  Treat `kernel_thread_create()` as a
  construction primitive, not proof that repeated S-mode worker sleep/resume is
  safe for reclamation work.
- Until a cooperative S-mode continuation primitive saves the current worker
  `pc`, `sp`, and preserved registers into `thp->reg` before `kernel_exit()`,
  `kernel_thread_create()` must remain unused by live worker call sites.  The
  reason is architectural: U-mode traps refresh `thp->reg`, but S-mode traps
  return from a stack frame and leave the saved thread context stale.
- `kernel_thread_checkpoint(CPU_REGISTERS *)` is that primitive.  A worker that
  will block through `kernel_exit()` must call it at the handoff point and treat
  return value `0` as the pre-dispatch path and return value `1` as the resumed
  path.  The measured thread-reclaim worker uses this shape before
  `unready_release_and_exit()`.
- The first live worker is intentionally narrow: it runs only after the exiting
  thread has woken its joiner, removed itself from `prp->threads`, and completed
  CPU/name cleanup.  It owns only the RCU grace period, thread-pin drain,
  no-`pc_curthread` confirmation, `object_free()`, process notification, and the
  temporary process pin release.

Implementation constraints:

- Per-hart RCU-sched quiescent-state and waiter fields live in `struct pcpu`.
  Do not move them back into compact `PROCESSORS_MAX` arrays; frequent QS
  publication would make every hart write different words in the same cache
  line, forcing cache-line ownership to bounce between harts.
- `rcu_sched_quiescent()` first checks whether this hart has already published
  the current epoch.  If so, it returns without rewriting the per-CPU QS slot.
  Otherwise it release-stores the new QS epoch; waiters acquire-confirm matching
  QS epochs.  Epoch advances are rare relative to syscall exits, context
  switches, and idle entries, so this is the main hot-path fence bypass.
- `rcu_sched_advance()` relies on `atomic_add_value()`, which is
  `amoadd.w.aqrl` on RISC-V.  The `.rl` half keeps earlier unpublication stores
  before the epoch advance, so do not add a redundant `cpu_smp_mb()` before the
  AMO.
- `rcu_sched_synchronize_detail()` must publish `pc_rcu_waiting = 1` before the
  Store-Load fence.  The waiter store is a release publication because peers
  treat `pc_rcu_waiting != 0` as a quiescent shortcut.  The following
  `cpu_smp_mb()` is still the Store-Load ordering point before this CPU scans
  peers and possibly advances the epoch.
- The final `in_kcall_dec()` zero transition keeps a full fence before the
  release-store zero.  This is the quiescent-state publication for readers
  leaving a kernel call.
- The pre-wait peer scan is single-pass and uses acquire loads.  The wait loop
  uses relaxed loads for repeated negative checks, with acquire confirmation
  only when a peer appears to be waiting, outside a kernel call, or already
  published at/after the target epoch.  The acquire confirmation belongs at the
  quiescent decision point, not every spin iteration.  Exponential backoff may
  be added only as a measured contention reduction on top of this relaxed-probe
  pattern; it must not remove the final acquire confirmation.
- RCU spin waits use the raw RISC-V PAUSE hint encoding so older assemblers can
  build the tree without recognizing the `pause` mnemonic.
- Do not use `pc_idle_thp` as an RCU-sched quiescent shortcut.  Idle code may
  run deferred kernel work before publishing its explicit idle-entry QS, so
  grace-period waiters must rely on the confirmed zero `in_kcall` transition
  or a later published QS epoch.
- Do not add an expedited RCU IPI until the receiver-side operation is specified
  here as bounded trap work.  The sender must publish the request before the IPI,
  the receiver must acquire the request before acknowledging, and reference-host QEMU
  measurements must show lifecycle p99 improvement without adding interference
  to isolated RT harts.  The sender may target only harts that remain
  non-quiescent for the requested epoch, and must skip isolated RT harts already
  executing in U-mode because they are quiescent for kernel RCU-sched.
- A sleeping RCU grace-period wait needs an explicit wait queue and a
  peer-QS-publish wake source.  It may be used only by sleep-safe callers after
  a short bounded spin phase; hard-RT deadline paths must still use async
  reclamation rather than blocking on the grace period.
- Grace-period batching must enroll a writer before the shared epoch advances.
  A writer that unpublishes after an epoch is already active cannot assume that
  epoch covers readers that observed the old pointer.
- `rcu_assign_pointer()` is a publish operation, not a full synchronization
  point.  On RISC-V it uses a store-release fence before the pointer store so
  object initialization is visible before publication without imposing a full
  `fence rw, rw` on later loads.

Priority-inheritance work must not dereference an owner thread after its RCU
read section closes unless another lifetime rule pins it, such as an owning
spinlock or an explicit reference.

| Object family | RCU instance | Read-section bound | Lifetime beyond `rcu_read_unlock()` |
| --- | --- | --- | --- |
| `kernel/include/rcu.h` RCU-sched backend | each declared `rcu_t` instance | `rcu_read_lock()`/`rcu_read_unlock()` are compiler barriers; context switch, U-mode return, dead-thread exit, idle, and the final `in_kcall_dec()` zero transition mark quiescent states. | No object lifetime is extended after unlock; writers free only after `rcu_sched_synchronize()` from a non-deadline path. |
| Process vector lookups | `process_rcu` / vector publication paths | Enter before loading the vector slot, exit after copying the pointer fields needed for the operation. | A later dereference needs `vec_slock`, a target object lock, or a future ref/pin API. |
| Channel/connect `rcvid` lookup | `connobj_rcu` | Enter before decoding and loading `channel_t`/`connect_t`; exit only after the object lock or queue membership pins the object. | `chp->lock`/`cop->lock` plus queue membership pins message-path objects; destroy waits for the grace period. |
| Sync owner lookup for turnstiles | `tthread_rcu` or replacement thread pin API | Enter before resolving `(pid, tid, generation)` to `thread_t *`; exit after acquiring an explicit thread pin or while the owner field is still protected by the chain lock. | Turnstile PI must add `tthread_pin()`/`tthread_unpin()` before using owner pointers after lookup. |
| Debug/proc snapshots | relevant object RCU plus local copy | Snapshot under read section, then format copied scalar data outside. | Debug output must tolerate stale/missing names and never extend lifetime by pointer reuse alone. |

## Connection Reference Count

`connect_t.links` counts client references: one per client fd slot plus one per
sender that is blocked on, or about to publish itself on, the connection.  It
reaches 0 exactly once.  `ksys_msg_sendv` takes its reference with
`connect_link_get()` (a compare-and-swap that fails on 0) right after the lookup
and before the sender is linked on any queue; a send that does not block drops it
again.  `ksys_connect_detach` unpublishes the fd slot under `vec_slock` and then
drops through `connect_client_drop()`.  Because every sender is counted before it
is visible, the party that sees the last drop is unique: the detaching client, or
the reply-side drop of the last blocked sender.  A sender that looked the
connection up before the last fd was closed either wins the race (its reference
makes the client's drop not the last) or loses it and fails with `EBADF`.  The
last drop of a `CHF_DISCONNECT` connection sends the disconnect notice, after
which the server's `ConnectDetach` of its scoid frees the soul; a connection
whose channel is already destroyed is freed by the dropping client.  Any other
last client drop leaves the soul to channel destroy or process exit.

### Process-exit kill of a reply-blocked client

`kext_exit_destroy_threads` second pass kills a client still in `STATE_REPLY`
or `STATE_SEND` (its server is not the caller, or has not replied) by unlinking
and marking it dead itself rather than through `force_ready`.  It follows the same rule: unlink from the queue under
`chp->lock`, after the `WAIT_PENDING` spin and with the state, `blocked_on` and
`MSG_DELIVERY` rechecked under the lock (a claimed thread belongs to its claimer
and is left to `thread_destroy`); clear the server's `client` back-reference and
the client's `args.ms.server` in the same lock window, because the server's next
receive stores through `client->args.ms.server`; then drop the link.  A
`STATE_SEND` client is on the priority-list `send_queue` (`pril_rem`), a
`STATE_REPLY` client on the plain `reply_queue` (`LINKPRIL_REM`).

Two more rules keep a dead thread off every queue.  The `WAIT_PENDING` spin comes
before `blocked_on` is read (here and in the `STATE_REPLY` and `STATE_SEND` arms of
`force_ready`):
`block()` zeroes `blocked_on` while the sender is already linked, so a read ahead
of the spin sees a transient NULL, takes the "exchange already completed" path,
and the thread is destroyed while still linked.  And the `STATE_DEAD` stamp is made
in the same `chp->lock` window that rechecked the state and unlinked the thread,
never from the lock-free state read at the top of the loop: that read is stale by
the time of the store, and a thread that has since moved on (to a receive queue,
say) would be left dead and linked.  A thread the pass could not unlink is not
stamped; it goes through `thread_destroy`, whose quiescence gate and
`force_ready` already handle it.  Separately, `force_ready_msgxfer` loads each
hart's `get_active_thread()` once: the slot clears to NULL when that hart blocks
into idle, so testing it and then calling the accessor again can fault.  The
server's `client` link and the client's `args.ms.server` are cleared by the
server's receive as two separate stores, so a `force_ready` of the client may see
the pair half-cleared; it clears the server's link with a compare-exchange against
the client and does not treat the half-cleared state as corruption.

### Unblock notification of a reply-blocked client

When `force_ready` aborts a `STATE_REPLY` client whose channel has `CHF_UNBLOCK`
(a REPLY timeout, a signal), it does not unblock the client: it queues one
`_NOTIFICATION_CODE_UNBLOCK` for the server and leaves the client reply-blocked
until the server replies.  The notification is sent once
(`ITF_UNBLOCK_QUEUED`).  A multi-threaded server can therefore receive it on one
worker while another worker is still dispatching the original request, before
that request has parked the client; the notification handler then finds nothing
to unblock and the client is never woken (a `sem_timedwait` on a named semaphore
hung this way, `CPUS=1`, when the deadline expired in the window between the
server's receive and its queueing of the client).  Two rules close it:

- The kernel stores `FLG_THR_UNBLOCK_REQ` (visible to the server as
  `RTMUX_MI_UNBLOCK_REQ` in `msg_info`) before the notification is delivered, not
  after, and clears it if delivery fails.  A server that sees the notification
  therefore always sees the flag.
- A server that parks a client for a later reply must, under the lock that its
  unblock handler also takes, check `RTMUX_MI_UNBLOCK_REQ` before queueing and
  fail the request with `EINTR` instead of queueing when it is set.  Either the
  unblock handler runs second and finds the queued client, or the park runs
  second and sees the flag.  `servers/roottask/path/namedsem.c`
  (`sem_service_read`) follows this; other servers that park clients (those with
  an `unblock` handler) need the same check.

## Message Bridge Bits

`msg_send()`, `msg_receive()`, and `msg_reply()` use per-thread bridge bits and
hardware fences to close cross-CPU state-transition windows.  The rule is:
publish queue membership and thread state, execute `cpu_smp_mb()`, then clear
the bridge bit.  Wakers that spin on the bridge bit must execute the matching
fence before trusting queue links or thread state.

Do not replace this with lock-free pointer reads unless the replacement states
the publish, observe, and lifetime points with the same precision.

`tests/litmus/ipc-wait-pending-mp.litmus` (gate: `make check-ipc-litmus` /
`scripts/check-ipc-litmus.sh`) is the RVWMO proof obligation for this handshake:
it models the waiter's publish-fence-clear and the waker's spin-fence-read and
proves the lost-wakeup outcome (waker sees the bridge bit cleared but reads
stale state) is unreachable.  A paired `-nofence` control removes the two
ordering `cpu_smp_mb()` fences (the waiter's publish fence before the clear and
the waker's fence after observing the clear) and must show the same outcome
becomes reachable, so the gate also proves those fences are load-bearing, not
accidentally passing.  The waiter's post-clear store-buffer drain fence is a
liveness concern and is outside this stale-state proof.

Two CBMC harnesses (`make check-ipc-cbmc` / `scripts/check-ipc-cbmc.sh`) cover
the sequential resource and authority safety the litmus cannot: `ipc-pool-pairing`
model-checks the soul freelist alloc/free pairing that backs the IPC message
pools (no double hand-out, no double free, freelist integrity, no leak), and
`ipc-cap-authority` model-checks the `cop != object` check/use recheck in
`inline_lookup_connect` (only the capability-authorized connect_t is ever used;
a slot reused by a concurrent revoke cannot be reached through a stale
capability).  Each harness also builds under `-DPLANT_BUG` and must fail, so the
green result cannot come from vacuous assertions.

A Z3 SMT model (`make check-cap-smt` / `scripts/check-cap-smt.sh`,
`tests/smt/cap-algebra.smt2`) covers the capability rights/revocation algebra of
`kernel/cap/table.c` at the spec level: rights attenuation, same-table
delegation shares the revocation group, cross-table transfer mints a fresh
(nonzero, not-live) group, an unpinned revoke kills the whole group, and
independent roots stay isolated.  Each invariant has a reachability check (`sat`)
and a negation check (`unsat`), and a planted-bug companion breaks each operation
and must go `sat`, so no invariant passes vacuously.  It is a spec-consistency
model of the group/rights/revoke shape, not a full cap-table proof (generation
stale-handle rejection, `pin_count` lifetime, and delayed destruction are out of
scope).

A second Z3 model (`make check-srr-pi-smt`, `tests/smt/srr-pi-safety.smt2`)
covers the N-hart SRR + bounded-PI safety invariants: no priority leak (deboost
returns to base), a boost never lowers the owner below its waiter, the PI walk
is bounded (a cycle or over-deep chain returns `EDEADLK`), no thread runs on two
harts, and no transition acts on a stale (recycled) receiver.  Each invariant
maps to its concrete function, linearization point, and covering `rt-check` /
`CONFIG_LOCK_DEBUG` probe in the section 13 refinement table in
`docs/pi-pc-design.md`.

A third Z3 model (`make check-lifecycle-smt`, `tests/smt/lifecycle.smt2`) captures
the object-lifecycle teardown contract as an inductive transition system: a dead
process holds no threads, channels or caps; a zombie has no threads; `nfds`
counts client attaches only (server-side `scoid` slots do not) and never
underflows; `connect_t.links` equals its holders and stays non-negative;
`blocked_on` is set iff the thread is blocked and never names a freed
connection; an rcvid pairs with a blocked sender of the owning process (a contract
for the callers of `lookup_rcvid`; the generation is a model abstraction).  It
records the invariants three fixed teardown bugs violated (9717d81c,
95b4205a, 3607e29a).  It is sequential contract capture, not a live-race gate:
refcount atomicity, RCU lifetime and memory ordering stay with CBMC, litmus and
runtime tests, and several invariants hold by construction of the sequential
steps (see the gap notes in the model header).  The gate also mutates the real model in place and requires an
invariant to flip, and checks that the planted file shares its operations.

Two Spin temporal models (`make check-temporal`, `tests/spin/`) cover the
liveness the SMT and litmus models cannot: `ipc-rendezvous.pml` proves the SRR
rendezvous makes progress (a sender and receiver never both block forever with a
partner available, because of the under-lock re-probe), and `pi-walk-terminate.pml`
proves the PI boost walk terminates over any wait-chain shape (`EOK` at an
unblocked owner, else `EDEADLK`).  Each has a committed non-vacuity witness: with
the guard removed (`-DNO_REPROBE` / `-DNO_GUARD`) the property is violated, so it
is falsifiable, not vacuously true.

The wait-for graph is covered separately by `docs/ipc-deadlock-freedom.md`
(`make check-ipc-deadlock`): blocking server-to-server sends follow a strict send
rank table, a Spin model finds no deadlock over it, and a planted cyclic graph
must deadlock.

### Barrier audit

Every hardware barrier on the IPC fast path and the userland driver shared-ring
paths, with producer, consumer, protected data, required ordering, the fence
actually used, and whether it is narrowable.  A full `fence rw,rw` is kept only
where a narrower form is not proven equivalent, is dictated by the hardware
interface, or where narrowing has no measured payoff (`measure or revert`
applies before trading the uniform `cpu_smp_mb()` macro for a bespoke fence).

| Barrier (site) | Producer to consumer | Protected data | Required ordering | Fence used | Narrowable? |
| --- | --- | --- | --- | --- | --- |
| WAIT_PENDING publish (`kernel/ipc/fastmsg.c` 985 reply, 1123 send, 1983 receive) | blocking waiter to waker | waiter `state`, `blocked_on`, queue links | data stores before the bridge-bit clear (StoreStore) | `cpu_smp_mb()` before `ITF_CLR(WAIT_PENDING)` | yes to `fence w,w`, proven RVWMO-safe by `ipc-wait-pending-mp-weak.litmus`; not applied (no measured win, shared macro) |
| WAIT_PENDING observe (`fastmsg.c` 701, 1295, 1612, 2038; `kernel/ipc/notification.c:164`; `kernel/sched/dispatch.c` 1146 receive, 1179 send in `force_ready()`) | waker to waiter | same waiter state, read after the spin | bridge-bit load before the state loads (LoadLoad) | `cpu_smp_mb()` after the WAIT_PENDING spin | not proven: several of these sites also order a prior `MSG_DELIVERY` claim store (`START_SMP_XFER`) that a bare `fence r,r` would not; `ipc-wait-pending-mp-weak.litmus` covers only the isolated bridge-bit MP, so the full fence stays until a model including the claim handoff proves a narrower form |
| WAIT_PENDING drain (`fastmsg.c` 1994) | waiter store buffer to remote spinner | the bridge-bit clear itself | drain the clear so a remote spinner stops seeing the stale value (progress) | `cpu_smp_mb()` after `ITF_CLR` | no; a liveness/progress fence retained by implementation policy, outside the stale-state MP proof |
| NVMe SQ doorbell (`userland/dev-nvme/nvme-ctrl.c:206`) | driver to NVMe controller | submission-queue entry stores | SQE stores before the MMIO doorbell store | `fence w,o` | already narrow (device-store ordering, not a full fence) |
| NVMe CQ phase/CQE (`nvme-ctrl.c:239`, `nvme-ist.c:76`) | NVMe controller to driver / IST | completion-queue entry fields | phase-bit read before the CQE-tail reads | `fence rw,rw` | no; the source records that `volatile` and `__atomic_thread_fence` are insufficient for the DMA-coherent CQE reads on RISC-V |
| VirtIO ring (`virtio_mb()` = `__sync_synchronize()`, `servers/lkl/virtio.c:127`; called at `virtio.c:420`, `servers/lkl/host.c` 934/1085/1101) | LKL guest and delegated VirtIO device | virtqueue descriptor / avail / used rings | descriptor and avail publish before the used observe, per the VirtIO spec | full barrier | no; matches the VirtIO spec `mb()` requirement |

The WAIT_PENDING publish/observe fences are the ones `ipc-wait-pending-mp.litmus`
proves load-bearing.  The driver-ring fences are device/DMA-ordering barriers
whose strength is set by the hardware interface (MMIO ordering, DMA coherence),
not a CPU-to-CPU handshake, so a CPU-only litmus does not model them; their
rationale lives at the cited source lines.

| Handshake | Writer stores | Publish point | Reader acquire point | Guaranteed lifetime |
| --- | --- | --- | --- | --- |
| Lock rank checking | lock class and instance on per-CPU held stack | local interrupt-disabled critical section in `kspinlock_lock()` | same CPU checks stack before recording nested acquire | only debug metadata; no object lifetime guarantee beyond the lock itself. |
| RCU grace period | writer unpublishes pointer, advances epoch | `rcu_sched_synchronize()` after unpublish | peer harts publish zero with release in `in_kcall_dec()` or publish a later quiescent state; waiters acquire-load zero | storage can be recycled only after every live peer hart has crossed one of those quiescent points. |
| Send queues to receive | sender queue links, `STATE_SEND`, `blocked_on`, bridge bit | `cpu_smp_mb()` before clearing the bridge bit / making waiter visible | receiver spins or locks until bridge bit is clear, then observes queue links | sender remains queued or ready-owned until reply/error path removes it. |
| Receive blocks waiting for send | receiver `STATE_RECEIVE`, channel receive queue links, bridge bit | queue insert under channel lock plus `cpu_smp_mb()` before bridge clear | sender observes bridge clear before matching receiver | receiver thread remains pinned by queue membership until send/reply state transition. |
| Reply wakes sender | reply data and sender state transition | data copy and state update before readying sender | sender returns through kernel exit after scheduler acquire | reply buffer lifetime is caller-owned; kernel only guarantees copied bytes and final status. |
| Notification delivery | notification payload, target queue link or pending bit | queue/pending update before readying target | target observes under channel lock or scheduler acquire | notification object stays queued until receive consumes it. |

RTMux notification = the single async-delivery primitive unifying what Linux
splits across threaded-IRQ wakeups, eventfd, and completions. It is
value-carrying and queued, pulled by the receiver via MsgReceive; it is not a
POSIX signal and not a lossy badge.

## IPI Commands

IPI bits are defined in `kernel/include/smpswitch.h` and handled in
`arch/riscv/trap-handler.c`.

| Command | Receiver action | Ordering rule |
| --- | --- | --- |
| `IPI_RESCHED` | leave idle/WFI or return through scheduler path | Sender must publish runnable state before `SENDIPI()`. |
| `IPI_TIMESLICE` | scheduler tick handling | Same as reschedule. |
| `IPI_TLB_FLUSH` | `sfence.vma`, then completion-counter release | Sender must publish page-table changes before the IPI and wait for the counter when freeing/reusing storage. |
| `IPI_TLB_SAFE` | acknowledge safe TLB point | Used as a round trip; caller waits for bit clearance. |
| `IPI_CLOCK_LOAD` | reload local timer | Timer state must be visible before send. |
| `IPI_PARKIT` | park/quiesce receiver | Crash/shutdown-only paths. |
| `IPI_CRASH` | enter crash rendezvous | No normal locks may be acquired. |
| `IPI_CONTEXT_SAVE`, `IPI_CHECK_INTR`, `IPI_INTR_MASK`, `IPI_INTR_UNMASK` | reserved or not-yet-implemented | New users must document the receiver side first. |

`send_ipi()` ORs the command into the target pending word and sends an SBI IPI.
If a caller depends on data written before the command, it must provide the
appropriate barrier before or through the atomic operation it uses.

| IPI | Writer stores | Release before trigger | Receiver acquire | Notes |
| --- | --- | --- | --- | --- |
| `IPI_RESCHED` / `IPI_TIMESLICE` | runqueue links, priority bitmap, preemption request | scheduler unlock or `fence w, w` before SBI send | scheduler lock acquire or `fence r, rw` at IPI entry | `__cpu_membarrier()` is load-bearing when no lock acquire pairs with the sender. |
| `IPI_TLB_FLUSH` | page-table entries and shootdown request | C11 release fence before send | IPI handler reads command after acquire, executes `sfence.vma`, then increments `pc_tlb_flush_done` with release ordering | Page-table storage must not be freed until all target CPUs acknowledge. |
| `IPI_TLB_SAFE` | safe-point request generation | release before send | acquire before publishing acknowledgement | Used as a rendezvous; caller waits for completion before recycle. |
| `IPI_CLOCK_LOAD` | per-CPU timer deadline | release before send | acquire before reading timer state | Redundant if timer state is protected by a lock acquired by both sides. |
| `IPI_CRASH` / `IPI_PARKIT` | crash reason and rendezvous state | `fence w, w` before send | `fence r, rw` at trap entry | No normal locks on receiver. |

PTE manipulation records `tlb_flush_pending` while the state machine is
serialized by `as_slock`, then releases that spinlock before waiting for
remote acknowledgements.  The completion helper performs the local `sfence.vma`,
sends `IPI_TLB_FLUSH` to every other hart, and waits until each target's
`pc_tlb_flush_done` counter advances.  The receiver increments that counter
only after its own `sfence.vma`, so callers may free or recycle page-table
storage only after the helper returns.

The current regression suite includes a stale-map check: after `munmap()`, a
spawned helper touches the old address and must die with `SIGSEGV`.  QEMU CI is
currently single-hart, so the implementation contract is covered by code review
and build/runtime smoke; a measured multi-hart latency provenance block is still
required before making a hard realtime release claim.

## Trap Context

Trap and hard-IRQ paths may acknowledge, mask, record a claim, set pending
state, send an IPI, and ready an already-known waiter through the established
kernel path.  They must not allocate, block, call libc, perform
scheduling/IPC state transitions that can sleep, or perform unbounded queue
walks.

| Entry point | Allowed | Forbidden |
| --- | --- | --- |
| External interrupt trap | claim/mask, record `interrupt_t` fire, wake attached IST/event | allocation, device queue draining, IPC send/reply work, unbounded demux loops |
| Timer interrupt | bounded timer expiry accounting, ready already-known timeout waiters | arbitrary callbacks or I/O-service policy |
| IPI trap | consume command bits and run the command-specific bounded action | normal message-path blocking or allocator paths |
| Fault/ecall trap | enter normal kcall/fault machinery after context save | treating hard-IRQ restrictions as relaxed before leaving trap context |

Device-specific draining belongs to the user-mode IST.  See
`docs/irq-contract.md`.

## Management IPC Lock State

No synchronous management IPC while holding roottask object locks.  The current
§7 management graph is still collapsed into roottask, but the target graph is
`coresrv -> namesrv`: process/memory lifecycle may ask namespace questions, and
namespace code must not call back into process/memory while holding namespace
state.

Current rule: snapshot inputs under `path_mutex`, drop the lock with
`path_node_complete()` or `pthread_mutex_unlock(&path_mutex)`, then call
any blocking IPC/open path and revalidate with generation or
`namespace_version` before binding the result.  The host
`test-management-ipc-lock.py` check rejects obvious new `msg_send*`, `open`,
`sopen`, `_connect`, or `name_open` calls while `path_mutex` is held.

## Sender Identity and Process Snapshots

Sender identity is kernel-derived, never taken from message payload.
`msg_receive()` reports the sending process from the receive-time `connect_t`
(`cop->process->pid`), so a client cannot forge a peer pid by embedding one in
the message body.  Endpoint authority follows the same rule: a `MsgSend*` is
authorized by the generation-tagged endpoint cap behind the descriptor, not by
any caller-supplied identifier.

Cross-CPU process snapshots (`kext_proc_query`, the `fault_siginfo` exit
status) are lockless advisory reads post-BKL.  A field mutated by the target on
another CPU — `fault_siginfo` while that process takes a fatal fault,
`num_active_threads` during teardown — may be observed torn.  This is
acceptable because every snapshot field is plain data with no embedded pointer
the reader dereferences, so a torn value is at worst stale, never unsafe.  Do
not add a lock for a single field here; the whole snapshot shares the
advisory-read contract.

## Scheduler

`kdispatch_t` is per-CPU runqueue state.  Type and API names follow POSIX and
Linux-kernel convention (snake_case, `_t` suffix).  Scheduler code mutates
dispatch lists under the target dispatch lock.  Cross-CPU steals and
remote wakeups must lock the target dispatch and use a stable ordering when
more than one dispatch lock is needed.

| RTMux state/policy | Linux-familiar analogue | Notes |
| --- | --- | --- |
| `STATE_RUNNING`, `STATE_READY` | `TASK_RUNNING` | Ready and currently running share Linux's runnable analogue. |
| `STATE_RECEIVE`, `STATE_REPLY`, `STATE_SEND`, `STATE_INTR` | blocking wait states | Exact wake source is RTMux IPC/interrupt state, not Linux wait queues. |
| `STATE_NANOSLEEP` | timed interruptible sleep analogue | Timeout path must preserve bridge-bit and scheduler ordering. |
| `SCHED_FIFO`, `SCHED_RR`, `SCHED_OTHER`, `SCHED_SPORADIC` | POSIX scheduler policies | Names are already POSIX; RTMux policy enums stay unchanged. |

## Turnstile Prerequisite

The sync chain is FreeBSD umtx-style: hashed wait queues keyed by the user
`sync_t *`, with per-key priority-ordered wakes.  FIFO order is preserved
across equal-priority waiters on the same key; unrelated keys colliding on
the same physical chain remain isolated.  `sync_dequeue_head()` scans the
chain for the matching key and returns the highest-priority waiter
explicitly, so the wake decision does not depend on insertion order being
maintained by `sync_enqueue()`.

Priority-inheritance and priority-ceiling work layers turnstile state on top
of this chain.  Owner tracking, boost propagation, deboost, and the
ceiling-enforcement gate are specified in `docs/pi-pc-design.md`; any field
that survives a turnstile change must obey this RCU and lock-order contract
before the kernel advertises realtime POSIX mutex protocol compliance.

## IPC capability-use window (Spectre-V1 stance)

Scope honesty first: the authority path on the IPC send fast path is NOT
branchless, and nothing here claims it is.  The shared validator
cap_lookup_pin_typed (kernel/cap/table.c), reached by
rtmux_cap_lookup_pin_context, compares index against table->nslots and then
loads table->slots[index]; that magnitude compare is a textbook
Spectre-V1 bounds-check-bypass surface.  RTMux leaves it unmitigated on purpose.
Per the TODO "Open Questions" Spectre stance, no speculation barrier, no index
mask, and no speculative-load hardening are added here: this is a named gap, not
a silent one, and it will only be revisited behind a measured threat that
justifies the fast-path round-trip cost.

What the section 14 gate does enforce is a regression bound, not a proof.
scripts/check-cap-v1-branches.py disassembles the built kernel objects and
freezes the ordered conditional-branch signature of each audited window, so a
fast-path edit that adds a NEW data-dependent branch to those windows (a second
bounds check, a signedness test, or a secret-dependent branch inserted between
capability validation and the slot->object load), or that reorders the existing
branches, changes the signature and is caught.  The signature is ordered rather
than a bare count so a swap that leaves the count unchanged still fails.
`make check-cap-v1` runs it after a build; a planted bounds-check branch changes
the signature and fails the gate.  The gate is calibrated to the make defconfig
build with the in-tree riscv-none-elf toolchain; a deliberate toolchain or
config change that alters codegen is a review event, not an automatic pass.

Coverage boundary, stated plainly: the gate freezes the two standalone callee
bodies, the shared authority validator and the object lookup, because those are
where every mistrainable magnitude (bounds) compare on the send authority path
lives.  The caller-side glue in inline_lookup_connect (kernel/ipc/fastmsg.c) is
static inline with no standalone symbol, so its branches (the generation
discriminator that picks the cap path over the legacy coid path, the
side-channel-vs-fd vector select, and the cop != object recheck after the
lookup) are inlined into ksys_msg_sendv and are not independently frozen here.
That glue adds only equality and select branches, no magnitude compare, so it
carries no Spectre-V1 bounds-check-bypass surface today.  A future edit that
introduced a bounds compare into that inline glue would not be caught by this
gate; freezing it would mean auditing the whole ksys_msg_sendv body, whose
unrelated churn would make the gate noisy.  That trade is deliberate: the gate
covers the mistrainable primitives, not the full inlined send path.

Audited windows and their frozen branch classification:

| Window | Branch | Role | V1 disposition |
| --- | --- | --- | --- |
| cap_lookup_pin_typed | 3 zero tests (table, slots, object non-NULL) | argument sanity | non-secret, no slot access reached yet |
| cap_lookup_pin_typed | index == 0, index >= nslots (1 bgeu) | slot bounds | the one accepted V1 bounds compare per this window |
| cap_lookup_pin_typed | 2 lock spin tests | cap_lock acquire | control flow, no authority datum |
| cap_lookup_pin_typed | rights == 0 | reject empty-rights lookup | authority check, precedes use |
| cap_lookup_pin_typed | state == LIVE, generation match, object type, rights subset | authority validation | must precede the slot->object load; no branch sits between the last of these and the load |
| cap_lookup_pin_typed | pin_count == UINT32_MAX | refcount saturation | liveness, not authority; still precedes the object publication |
| vector_lookup2 | id >= nentries (1 bgeu) | vector bounds | the accepted V1 bounds compare on the post-cap object lookup |
| vector_lookup2 | slot-empty test, tag test | membership | non-secret vector metadata |

The invariant the gate protects, within the two audited callee bodies: the only
bounds compares in these windows are the two recorded above, and the authority
checks (state, generation, rights) remain the last data-dependent branches
before the validated object is read.  A new or reordered branch in either window
is a gate failure that a reviewer must justify here before regenerating the
golden with the script's --emit mode.
