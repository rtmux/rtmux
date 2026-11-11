# Typed process/thread capabilities: prerequisite design contract

Status: design only. This document specifies the three prerequisites the roadmap
requires before the typed per-object process/thread capability direction (Rung 3)
may be opened as an implementation task. It does not authorize kernel code; it
resolves the open questions the two adversarial reviews raised in
the internal capability-microkernel research notes (section "Review
findings") from problem statements into concrete designs, so a future session can
make the open-or-defer decision on evidence rather than on hand-waving.

The gate: do not start Rung 3 until (1) the
cap-distribution path, (2) the soul-reuse ordering invariant, and (3) the revoke
fan-out cost are specced, and the revoke-latency evidence row is re-measured for
the process/thread teardown frequency on the reference host. Sections A, B, C below specify
1/2/3. Section D specifies the ABI change all three sit on. Section E resolves
the one thing that cannot be specced away: the measurement is circular, because
there is nothing to measure until a minimal mechanism exists, so E defines the
minimal measurement spike that produces the reference-host number without committing to
the full migration.

## A. Cap-distribution path

Problem (review item 1): mint-at-creation assumed the process tree is already a
capability-derivation tree reachable by IPC transfer. It is not. Every spawn is
mediated by the roottask proc service (servers/roottask/proc/spawn.c), the kernel
creator is always roottask, parent_pid is a caller-supplied argument, and every
child is minted priv_caps = RTMUX_PRIV_CAP_ALL. So there is no existing path that
hands a parent a capability over its child.

Design. The child process capability is minted by the roottask proc service and
delivered to the requesting parent in the spawn reply, reusing the existing IPC
cap-transfer slots (up to four caps per message). Concretely:

- process_create already returns new_pid at spawn.c:210 and stores it in
  lcp->ctx.pid. At the _PROC_SPAWN_DONE handler (spawn.c:412), after startup
  commits, the mint happens and the child process capability rides the spawn reply
  to the originator.
- The mint is a new privileged proc-service call, and it must run in S-mode.
  Roottask does not hold a usable reference to the child process_t: root_proc(pid)
  returns roottask-side bookkeeping, not the kernel heap object, and roottask is
  forbidden from dereferencing kernel pointers. So the new call resolves and pins
  the process by pid inside the kernel (the lookup_pid path already turns a pid
  into a process_t under the right generation check) and calls rtmux_cap_alloc with
  obj_type = RTMUX_CAP_TYPE_PROCESS and full process rights (INSPECT, MAKE_THREAD,
  CONTROL, KILL) into roottask's own table. Roottask never sees or fabricates the
  object pointer; it names the target by pid. This is the real new distribution
  plumbing the review flagged; scope it as its own reviewed change.
- The reply must actually carry the cap. The current spawn reply is a zero-length
  msg_replyv(..., 0, 0) at spawn.c:437, which transfers nothing. Cap transfer on a
  reply only happens when the reply payload is an _IO_NATIVE header carrying nxfers
  transfer descriptors, which ksys_msg_replyv installs at fastmsg.c:2230 and
  fastmsg.c:2261. So Section A requires a concrete spawn-reply protocol change: the
  _PROC_SPAWN_DONE reply becomes an _IO_NATIVE payload with one transfer descriptor
  naming the minted process cap, plus caller-side ABI handling to receive it. That
  reply-format change, not just the mint, is part of this prerequisite.
- Backward compatibility: a parent that ignores the returned cap loses nothing;
  authority still flows through priv_caps until an ambient check is actually
  deleted. The cap is additive until Section D's typed lookup lands and thread.c:91
  starts consulting it. Older callers that expect the zero-length reply must keep
  working, so the _IO_NATIVE reply format has to be opt-in or version-gated.

Not in scope: runtime delegation of a cap over an already-started process. That is
a separate design; the mint happens once, at the spawn reply, to the parent only.

## B. Soul-reuse ordering invariant

Problem (review item 4): caps key on the raw object pointer. A process_t carries an
independent liveness tag (pid = PINDEX composed with pid_unique, bumped on
destroy). If a dead process_t soul returns to process_souls and is reused before
the revoke scan that names it completes, the scan matches the new process's
address: mis-revoke, or a stale cap resolving to a live different process.

Design. The invariant is identical in shape to the connect_t invariant just
centralized in connect_soul_free (kernel/ipc/connect.c): object-death revoke
completes before the soul is freed, and the allocator never hands back an address
with live caps naming it. The enforcement point already exists and is currently
unguarded: process teardown frees the soul at kernel/ext/process.c:542 with
object_free(NULL, &process_souls, prp) and no cap fan-out before it. Thread
teardown has the analogous soul-free sites at kernel/thread/lifecycle.c:641 and
kernel/thread/lifecycle.c:1906. The fix mirrors the connection path:

1. Before object_free at process.c:542, fan out a revocation over every process
   table for caps naming prp, using the same rtmux_cap_revoke_object walk
   endpoint_cap_revoke_if_present uses.
2. Thread teardown gets the same treatment at lifecycle.c:641 and :1906 before a
   thread_t soul returns to its pool.

Cap-pin lifetime is the subtle part the connect_t case did not face. A revoke marks
matching slots dead or revoking, but rtmux_cap_revoke_object_context does not wait
for a concurrent lookup that has already pinned the slot and is about to dereference
the object. Cap pins are separate from the existing process_t / thread_t lifetime
pins. So the rule is: a typed lookup that resolves a process or thread cap must
promote the target to a real object pin (the object-pin path teardown already
drains: process_wait_unpinned in kernel/ext/process.c for a process_t, and
tthread_wait_unpinned in kernel/thread/lifecycle.c for a thread_t)
before it drops the cap pin and touches the object. Object-death teardown then drains
those object pins as it does today, and the cap fan-out only has to guarantee no NEW
lookup can pin a revoked slot. Without this promotion, a lookup holding only a cap pin could
dereference an object being freed at process.c:542, a use-after-free the cap
generation alone does not close.

The cap generation, not the pid generation, stays the authority tag; lookup must
never conflate them. Steps 1 and 2 are a strict generalization of code already
shipped for connect_t; the cap-pin promotion rule is the genuinely new obligation.

## C. Revoke fan-out cost and the escape hatches

Problem (review item 3): endpoint_cap_revoke_if_present holds process_vector_slock
across a full process_vector walk, each entry calling rtmux_cap_revoke_object. The
cost is worse than a flat scan: a matching slot triggers cap_group_retire, itself
a full slot scan over the table. So
with P processes and a fixed S = RTMUX_PROCESS_ENDPOINT_CAP_SLOTS = 128 slots per
table, the bound is O(P times S) when at most one slot per table matches, and
O(P times S squared) in the worst case where a process delegated caps into every
table so every table has matching slots. S is a fixed constant today, not
proportional to P, so this is bounded but the constant is large and it runs with
process_vector_slock held. Rung 3 adds this to every process exit and thread reap, a
hotter frequency than connect_t death.

The design offers three options, cheapest first, decided by the Section E number:

- Shorten the interrupt-off window without new per-object state. The pain is
  holding process_vector_slock across all table scans; the cap locks themselves do
  not mask interrupts (cap-table.h:49). So snapshot the process pointers under
  process_vector_slock (pinning each so it cannot be freed), drop the slock, then
  run the cap-table revoke per process outside it. Same total work, far smaller
  non-preemptible window. This is the lazy fix and is preferred if the total work is
  fine but only the irq-off span misses budget.
- Keep the full scan as is. Acceptable if the reference-host re-measurement at
  process/thread teardown frequency stays within the interrupt-off budget even
  without the snapshot trick. This is the do-nothing option.
- Add a per-object back-index only if total work, not just the window, is too high.
  Each cap-bearing object carries an intrusive list of (table, slot) pairs holding a
  cap over it, appended on alloc/transfer-in and removed on revoke, turning the walk
  into O(caps-on-object). Cost: one list head per object, two list ops on the cold
  transfer path, and the list mutation under the cap-table lock discipline. This is
  the new machinery the proposal ruled out; it is justified only by a measurement
  showing the total work, not merely the window, exceeds budget, never speculatively.

The order is deliberate: the Section E number decides which option is needed. If
only the non-preemptible window misses budget, the snapshot-and-drop trick fixes it
with no per-object state; the back-index is only for a total-work miss. Do not build
the back-index before the measurement rules the snapshot option out.

Status: the snapshot-and-drop option is now implemented in
endpoint_cap_revoke_if_present (the shared fan-out), so the interrupt-off window is
already the O(1) per-process pin for both connection teardown and any future
process/thread teardown that reuses it. Only the back-index (a total-work reduction)
remains conditional on a measurement, and no measurement has shown total work to be a
problem, so it stays unbuilt.

## D. The obj_type ABI change (structural prerequisite)

Problem (review item 2): obj_type is free in sizeof (the slot has pad[2] at
kernel/include/cap-table.h:40) but not free in the ABI. Endpoint lookup treats any
nonzero-generation integer as an endpoint cap and checks only ENDPOINT_SEND;
get_or_alloc matches on (object, context, rights) with no type. So the moment a
single table holds more than one object type, a process cap and an endpoint cap with
coincidentally-equal (object, context, rights) collide.

Scope of the hazard, checked against the tree. The rtmux_cap_* code has two callers
today: the kernel endpoint table (process_t.endpoint_caps, kernel/include/objects.h:199)
and roottask memory-object caps (root_process_t.mem_caps,
servers/roottask/include/root-process.h:118, used at memcap.c:141 and :180). But those
are two separate rtmux_cap_table_t instances in two separate address spaces; no code
looks up an endpoint handle in mem_caps or a memory handle in endpoint_caps, and a
lookup only ever matches within one table. Every table today is therefore
single-type: endpoint_caps holds only endpoint caps, mem_caps holds only memory caps.
So there is no present-tense aliasing, and this is not a bug to fix now. One nuance
that sharpens why: endpoint_caps already holds caps over two object types today, both
connect_t and gated global channel_t (channel.c:184, message-core.c:80). They coexist
without a type tag because they share one rights class, the endpoint rights checked by
ENDPOINT_SEND, so a lookup that matches on rights cannot mistake one for a different
authority. obj_type earns its place only when a table gains a cap with a DIFFERENT
rights class, which happens exactly when Rung 3 mints process and thread caps (rights
INSPECT / MAKE_THREAD / CONTROL / KILL) into endpoint_caps (Section A). At that point an
endpoint cap and a process cap in the same table can share (object, context, rights)
yet mean different authority, and the type tag is what keeps them apart. obj_type is a
Rung 3 prerequisite, not a standalone fix, and mem_caps needs no cross-type defense
because it stays single-type.

Design. Spend pad[0] on uint8_t obj_type with values RTMUX_CAP_TYPE_ENDPOINT (0, so
recycled and zero-initialized slots keep today's endpoint meaning), _PROCESS, and
_THREAD. A _MEM value is optional hygiene for the isolated mem_caps table, not a
correctness requirement. Thread the tag through every slot-creating, matching,
copying, and lookup path in the kernel endpoint table, since that is the table that
goes mixed-type:

- rtmux_cap_alloc / rtmux_cap_alloc_context (table.c:216, 224): take and store the
  type.
- rtmux_cap_delegate (table.c:422): a delegate creates a new slot and must copy the
  source type, or a delegated cap loses its type identity.
- rtmux_cap_get_or_alloc_context (table.c:241): the match predicate becomes
  (object, context, rights, type); a same-address different-type object never
  aliases.
- rtmux_cap_transfer_to (table.c:483): copy the type field alongside rights.
- rtmux_cap_lookup_pin_context (table.c:277): reject a type mismatch before the
  rights check, so a caller presenting a thread cap where a process cap is required
  is denied on type, not on rights.
- rtmux_cap_has_object and rtmux_cap_revoke_object / _context (table.c:609): take or
  check the type, so once endpoint_caps is mixed-type a same-address different-type
  object is neither falsely reported present (global channel gate check) nor revoked
  together with an unrelated object that happens to share the address.
- The fast path in fastmsg.c must consult the type too: today it treats any
  nonzero-generation integer as an endpoint cap and checks only ENDPOINT_SEND, so
  once process and thread caps live in endpoint_caps, one presented on the send path
  must be rejected on type there, not just in the slow lookup.
- A static assert that sizeof(rtmux_cap_slot_t) is unchanged, so the type tag never
  silently grows the slot.

Because the enum value is 0 for ENDPOINT and slots zero-initialize, an un-updated
endpoint path degrades to endpoint semantics rather than misfiring; PROCESS and THREAD
are nonzero, so their creating and matching paths must be updated atomically with the
enum, or a process cap minted as type 0 would alias an endpoint cap in endpoint_caps.
The blast-radius argument protects only the endpoint tenant, not the new types.

## E. The measurement gate is largely already satisfied

The gate asks for a re-measured revoke-latency row for process/thread teardown. On
inspection that measurement mostly already exists, because the process/thread
teardown fan-out is not new mechanism: Section B reuses the exact
endpoint_cap_revoke_if_present walk that connection teardown uses today, calling the
same rtmux_cap_revoke_object over the same per-process 128-slot tables under the same
process_vector_slock. The per-operation revoke latency is a function of the process
count and the per-object cap population, not of which object type is dying, so it is
call-site independent. The existing Capability revocation latency section of
docs/rt-budgets.md already characterizes that cost (fan-out is O(num_processes times
S) common case, O(num_processes times S squared) under the adversarial pattern, plus
the RCU-sched drain that dominates the measured detach). That row is already the
revoke-latency artifact the capability-security evidence bar names, and it carries
over to process and thread teardown unchanged.

Two things the existing row does not settle, and how each is resolved:

- Teardown frequency. The Rung 3 concern was that process exit and thread reap fire
  more often than connect_t death. But frequency changes aggregate CPU cost, not the
  per-operation interrupt-off window that the realtime budget bounds. A higher rate
  of identically-bounded operations does not raise the per-operation latency, so it
  needs no new latency measurement; if aggregate throughput ever matters it is a
  separate throughput row, not a revoke-latency row.
- The adversarial O(S squared) window. This was the one shape the sampled row did not
  cover: a process holding up to 128 caps over a single dying object. It has now been
  taken off the interrupt-off budget: endpoint_cap_revoke_if_present pins one process
  at a time under process_vector_slock, drops the lock, and runs the cap-table scan
  with interrupts enabled (Section C's snapshot-then-drop option). The O(S squared)
  cost still exists as preemptible CPU work but no longer as a non-preemptible window,
  and the fix covers both connection teardown and any future process/thread teardown
  that reuses the same fan-out.

So Rung 3 does not need a bespoke throwaway measurement spike for the common fan-out
path: the identical-mechanism argument carries the existing measured row over to
process and thread teardown, and the adversarial O(S squared) shape is now off the
interrupt-off budget for both. What remains before Rung 3 opens is the mechanism in
Sections A through D (distribution path, the cap-pin-promotion rule, the obj_type ABI)
and their own correctness tests, not a new latency number.

## What stays gated

Rung 3 remains closed as an implementation task. The revoke-latency gate is met by
the existing measurement plus the identical-mechanism argument in Section E; what is
still unbuilt is the Sections A through D mechanism and its four-test evidence bar.
This document specifies the design; it does not authorize those kernel changes. The
four-test bar from docs/capability-namespace-feasibility.md (unforgeability,
delegation, revocation, revoke-latency) still applies to each object type before its
ambient check at thread.c:91 is deleted. thread.c:240 is not an authority site (it is
the thread_destroyall self-reap guard) and stays out of the migration set. The one
piece of present-tense work this analysis surfaced, independent of Rung 3, was the
snapshot-then-drop hardening of endpoint_cap_revoke_if_present for the adversarial
O(S squared) window; it is now implemented (see docs/rt-budgets.md) and takes that
scan off the interrupt-off budget.
