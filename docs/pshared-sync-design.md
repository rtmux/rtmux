# Process-Shared Mutex/Condvar Design

Status: draft for review (design-first; no kernel code written yet).
Goal: back `_POSIX_THREAD_PROCESS_SHARED` so a `pthread_mutex_t` / `pthread_cond_t`
living in a `MAP_SHARED` shm object coordinates threads in *different* processes,
then advertise the option honestly.

## Why this is a kernel change, not a libc flag

The active mutex/condvar machinery in `kernel/sync/wait.c` is keyed on the
*virtual* address of the user `sync_t`:

- `sync_chain_idx(sync)` hashes `(uintptr_t) sync` into one of `SYNC_NCHAINS`
  (128) wait chains (`wait.c:162`).
- Each `turnstile_entry` is keyed by the `sync_t *` VA itself (`.key`,
  `wait.c:145`); lookup compares that pointer.
- `wait.c` never resolves a physical address (no `vaddr_to_memobj`).

Two processes mapping the same shm page see the same *physical* page at
*different* virtual addresses.  A waiter in process A (VA_a) and a waker in
process B (VA_b) therefore hash to different chains, and even on a collision the
turnstile key comparison (`key == sync`) fails.  They never rendezvous.

An older physical-address design still exists in `kernel/sync/core.c`
(`sync_create`/`sync_lookup` resolve VA→paddr via `vm_ops->vaddr_to_memobj` and
hash a `ksync_t` by `(paddr, memobj)`), but it is **not** on the active
mutex/cond path — those kcalls now just stamp the type into the user lock word
(`ksys_sync_create`, `wait.c:1551`, "no per-sync kernel object to allocate").

So process-shared support requires giving the turnstile path a key that is
identical across processes for the same physical lock word.

## Memory model (confirmed) and why the redesign is necessary

Settled by reading `servers/roottask/mem/as/mmap.c`: user memory uses
**per-process address spaces** (`tmp->adp`, per-process `mm_map` regions with
independent `start`/`end`), not a global identity map — the "VA == PA identity"
in `arch/riscv/cpu-pte.c` is only the kernel direct-map and the `CPU_1TO1`
device/physical case (`mmap.c:592`).  So two processes genuinely can hold
different physical pages at the same virtual address, and a `MAP_SHARED` page
generally sits at *different* VAs in different mappers.  VA-keyed turnstiles
therefore cannot rendezvous a process-shared sync — the redesign is required,
not optional.

This also matches `docs/pi-pc-design.md:23`, which specifies the turnstile sync
key as "user `sync_t *` **plus owning process/address-space identity**".  The
active `sync_chain_idx` keys on the VA alone, i.e. it took the process-identity
shortcut — correct for the common case but the reason shared syncs (and, as a
latent pre-existing edge, two private mutexes that happen to share a VA across
processes) do not behave.  Keying shared syncs on the physical address supplies
exactly the missing cross-process identity.  (An "always key on physical address,
private included" variant would also incidentally close that latent private-VA
edge and drop the discriminator, but it changes private-mutex keying and so
carries regression risk; this design keeps private VA-keyed for zero risk and
treats the latent edge as out of scope.)

## Slice 2 decision: always key on physical address (Option B)

Slice 1 made the wait-queue key opaque (`sync_key_t`).  Slice 2 must make the
contended path produce a physical-address key for shared syncs.  Two options:

- Option A — key shared syncs on paddr, private on VA.  Needs the contended path
  to know a sync is PROCESS_SHARED.  There is no clean channel: `ksys_sync_create`
  discards the pshared attr flag, and the user lock word has no bit that is free
  across all three sync types — `count` has spare mutex bits but is the user
  value for a semaphore and the clockid for a condvar, and `owner` is
  WAITING+pid/tid (mutex) or a type tag (cond/sem).  A side table would need to
  be keyed by the very paddr we are trying to compute.  So A is awkward.

- Option B — key EVERY contended sync on its physical address (private and
  shared alike), resolved by `cpu_uvaddr_to_paddr` (slice 0).  No shared flag,
  no discriminator bit, no per-type special case.  A shared page yields the same
  paddr in every mapper (rendezvous works); a private page yields a per-process
  paddr, so two processes' private mutexes at the same VA get *different* keys —
  which also closes the latent same-VA cross-process collision the VA-only
  keying has (pi-pc-design.md:23).

Decision: Option B.  It is the "good taste" choice — it deletes the special case
rather than adding a flag channel.  Correctness rests on the key being
*consistent* for a given sync across its operations: within a process the same
VA resolves to the same paddr (page tables are per-process and the page is
pinned/faulted-in — `sync_probe` writes the lock word at every kcall entry
before any keying), and lock/unlock/wait/destroy on one sync all resolve the
same physical page.  It is NOT a zero-behavior change — private syncs are now
paddr-keyed — so the full QEMU mutex/condvar/semaphore suite is the correctness
oracle, not merely a smoke test.

Caller safety (Gemini + Codex reviewed the decision; both endorsed B):

- Take `as_slock` (the global page-table-manipulation lock, LOCK_CLASS_MEMPART)
  across the `cpu_uvaddr_to_paddr` walk.  Codex found the sync kcalls do NOT
  hold it today, yet the walk must be serialised against a concurrent `munmap`
  freeing a page-table page under it (else a freed-page-table deref / crash on
  buggy user code).  Acquire it standalone around the walk and release it
  BEFORE the chain lock, so it never nests under `LOCK_CLASS_TURNSTILE`.  Cost:
  the contended path serialises on a global lock for a ~3-read walk; acceptable
  (contended ops already block; the uncontested CAS fast path is untouched).  A
  per-process AS lock is a possible future refinement if it ever bottlenecks.
- On resolve failure (`-1`), ERROR the op (EFAULT) — do NOT fall back to the VA
  key.  Codex: a VA fallback on a race could split one sync's waiters (some
  paddr-keyed, one VA-keyed) and lose a wakeup.  After a successful `sync_probe`
  a failure means the mapping vanished mid-op (pathological / buggy user code),
  so erroring is correct.  `sync_destroy` instead just skips freeing the
  turnstile on failure (best-effort teardown), never erroring.
- An aliased mapping (one physical page at two VAs in one process) collapses to
  one key — correct, it is the same physical lock word.

Teardown consequence (slice 3): physical addresses are reused after a page is
freed, so a stale waiter left on a chain queue keyed by a reused paddr is more
dangerous than under VA keying.  Codex noted `memobj_destroyed` sweeps the OLD
`kernel/sync/core.c` synchash, not the new wait.c chain queues — slice 3's
teardown must hook the chain wait queues by physical-address range.

## Key idea: resolve a shared sync to a stable physical key

For a sync created `PTHREAD_PROCESS_SHARED`, key the wait chain and turnstile on
the `(memobj, physical-offset)` of the lock word instead of its VA.  That pair is
identical in every process mapping the shm object, so cross-process rendezvous
works with the *existing* turnstile queueing/wake logic — only the key changes.

Private syncs keep VA keying unchanged (no resolution cost, no behavior change).

### Key representation, and where it is stored (revised per review)

`sync_chain_idx` and the waiter identity currently use the `sync_t *` VA.  Note
(review finding, Codex): a non-PI contended mutex allocates **no** turnstile —
waiters live on the chain list keyed by `thread_t.args.mu.wait_key`.  So the key
abstraction must generalize the *waiter queue key*, not merely `turnstile_entry`.
The key:

```
struct sync_key {
    uintptr_t tag;      /* VA for private; physical address for shared */
    unsigned  shared;   /* 0 = private (tag is a VA); 1 = shared (tag is a paddr) */
};
```

- Private: `{ (uintptr_t) sync, 0 }` — bit-identical hash/compare to today.
- Shared:  `{ paddr, 1 }` — paddr from the page-table walk (above).

Chain index hashes `tag` and mixes in `shared`; equality compares both.  The
`shared` discriminator keeps a private VA from ever aliasing a shared paddr in
the identity-biased address space, even if the integers coincide.

BIGGEST RISK (review finding, Codex): a blocked thread outlives the syscall
stack — timeout, `force_ready`, and requeue all re-derive its key after the
stack is gone.  So the key must be stored **durably in the thread**
(`thread_t.args.mu.wait_key` widened from a VA to a `sync_key`), never as a stack
`sync_key *`.  Slice 1's whole job is this: decide the durable storage and route
every chain/turnstile/waiter lookup through it, with `obj == NULL` so behavior is
unchanged.

### Where resolution happens — no ABI bit (revised per review)

The first draft stashed a "shared" flag in the user lock word.  Rejected: review
found `count` already spends its high bits on protocol/type and low 24 on
recursion/ceiling, and `owner` spends the top bit on WAITING plus the rest on
pid/tid — there is **no free bit**, and a userspace-writable flag lets a corrupt
process flip key modes and strand waiters (a lost-wake DoS).

Instead, resolution is deterministic from the page itself.  On the *contended*
path only (after `sync_probe`, **before** taking the chain lock — faults must not
happen under `LOCK_CLASS_TURNSTILE`, Codex), resolve the lock word's memobj.  If
the page is backed by a **shared** memobj → key `{paddr, memobj}`; otherwise →
key `{VA, NULL}`.  Because lock, unlock-with-waiters, and condvar wait all resolve
the *same* page the *same* way, their keys always match — closing the
"unlocker computes a different key and wakes nobody" failure mode (Codex).  The
uncontended `mutex_try_acquire` fast path stays a pure userspace CAS: no
resolution, no cost.  Cross-process visibility of the `RTMUX_SYNC_WAITING` bit is
fine — it lives in the shared page and is published with the existing atomics /
membarrier.

## Resolution mechanism — kernel page-table walk (revised after reading arch)

Correction after reading the arch/VM source: the memobj route is the wrong one.
The kernel's `vm_ops->vaddr_to_memobj` (`kernel/mm/core.c:187`) is a stub
returning the singleton `_syspage_ptr` and `CPU_V2P(vaddr)` (a
kernel-linear-region bias, meaningless for a user page).  The *real*
vaddr→backing-object resolution (`map_isolate` → `mm->obj_ref->obp`) lives in
**roottask (U-mode)**, not in the kernel where the sync layer runs — reaching it
from the contended sync path would mean a roottask IPC upcall.  Rejected.

Instead, resolve the **true physical address** of the user lock word with a
kernel page-table walk.  RTMux gives each process its own page table (two
processes can hold the same user VA over different physical pages — that is
exactly why private syncs are VA-keyed), and `arch/riscv/cpu-pte.c` already has
`cpu_pte_lookup(pgdir, vaddr)`, a read-only walk returning the leaf PTE; its
`PTE2PA` plus the page offset is the physical address.  A `MAP_SHARED` page has a
single physical frame, so `pte_lookup` yields the *same* paddr for that lock word
in every mapping process — the natural cross-process key, with no memobj and no
roottask involvement.

So the "shared key" is simply that physical address; the `sync_key.obj` field
degenerates to a one-bit discriminator (private VA vs shared PA) so a private
VA can never alias a shared PA in the identity-biased address space.  Stability
assumption: physical frames do not move (no swap/migration today — matching the
existing `RUSH3: won't work once we start swapping` note); `MAP_SHARED` shm pages
are pinned.  Document it; revisit if demand paging of sync pages ever lands.

Resolver caller constraints (review, slice 0's `cpu_uvaddr_to_paddr`; enforced by
slice 2's contended path): the walk reads live page tables with no lock, so the
caller must (a) run in the calling user thread's own address space so `satp`
names it — a synchronous kcall from U-mode, never a kernel/idle thread or a
temporarily-switched `satp`; (b) hold the address-space guard that serialises
unmap (`as_slock`, `kernel/ext/manipulate.c`) — or otherwise keep the word
mapped — across the resolve, so a concurrent `munmap` cannot free an
intermediate page-table page under the walk; and it rejects supervisor-only
(`!PTE_U`) pages so a stray user pointer cannot key on a supervisor frame.  The
contended sync path already faults the lock word in (`sync_probe`) before
resolving, which is the natural place to satisfy (b).

## Robust teardown (revised per review — last mapper, not per-process)

A shared wait queue outlives the process that created it: process A can be
blocked on a shared mutex when process B (the creator) unmaps the shm.  So
teardown must fire on destruction of the **last** mapping of the physical page,
not on any one process's `munmap` — freeing a queue with A still on it would be a
use-after-free (Codex).  With the paddr key this is naturally range-based: the
existing `synchash_rem`/`memobj_destroyed` path already removes syncs by physical
address range `[addr1, addr2]` and force-readies their waiters, which is the
right shape for shared keys.  The hook must fire when the shm object's backing
frames are actually freed (last mapper gone), not on a single process's private
`munmap` of a still-shared page; a per-process unmap of a shared page only
detaches that process, leaving the shared queue intact while others map it.
Owner death on a shared mutex returns `EOWNERDEAD` to the next waiter.

## Priority inheritance across processes — scoped OUT of v1

The turnstile path implements PI: a blocked waiter boosts the mutex owner.  Across
processes the "owner" is a thread in another process, and PI propagation
(`sync_pi_propagate`) walks owner threads.  Cross-process PI raises hard
questions (priority meaning across scheduling domains, owner lookup by
`(pid,tid)` from a shared lock word).  v1 supports **non-PI** shared mutexes only
(`PTHREAD_PRIO_NONE`) and process-shared condvars; a shared mutex requesting
`PTHREAD_PRIO_INHERIT`/`PROTECT` returns `ENOTSUP` at init.  PI across processes
is a documented follow-up.  This keeps v1 off the most delicate turnstile code
(the PI propagation graph) while still delivering the common case.

## libc changes

- `pthread_mutexattr_setpshared` / `pthread_condattr_setpshared`: accept
  `PTHREAD_PROCESS_SHARED`, store the flag (stop returning ENOTSUP) — **only after**
  the kernel slice lands, so the attr never claims support the kernel lacks.
- mutex/cond init passes the shared flag through `sync_attr_t.flags` to
  `ksys_sync_create`.
- `pthread_mutexattr_setpshared` still rejects `PROCESS_SHARED` combined with a
  PI/PP protocol in v1 (ENOTSUP), matching the kernel scope.

## Slicing plan (revised; each slice independently built + SMP-regressed)

0. Kernel VA→PA resolver: a small helper that walks the calling process's page
   table (`cpu_pte_lookup` + `PTE2PA` + page offset) to return the true physical
   address of a user address, for use only on the contended shared path.  No
   roottask/memobj involvement.  Testable on its own (resolve a known mapping and
   confirm two mappings of one shm page yield the same paddr).
1. Kernel key abstraction, private-only: widen the durable waiter key
   (`thread_t.args.mu.wait_key`) and `sync_chain_idx`/turnstile lookups to a
   `sync_key`, routing the existing VA path through it with `obj == NULL`.  Pure
   refactor, zero behavior change; the whole existing suite is the oracle.  This
   generalizes the *waiter-queue* key (non-PI mutexes have no turnstile), not just
   `turnstile_entry`.
2. Kernel shared resolution: contended mutex lock, unlock-with-waiters, and
   condvar wait resolve the page's memobj (before the chain lock) and key on
   `{paddr,memobj}` when shared.  Non-PI only; a shared mutex requesting PI/PP is
   rejected at init.  Condvar wait resolves *both* the cv and the bound mutex key,
   keeps the canonical two-chain order, and the special-return reacquire resolves
   the same shared mutex key again.
3. Teardown: memobj-refcount-driven sweep of shared wait queues on last-mapper
   destruction; owner-death `EOWNERDEAD`.
4. libc: `setpshared` accept + flag plumbing to `sync_attr_t`; init-time
   PI/PP-on-shared rejection.
5. Test + advertise: a two-process shared-mutex/condvar test (fork + shm_open +
   mmap MAP_SHARED, lock/handoff/unlock across the fork), then advertise
   `_POSIX_THREAD_PROCESS_SHARED` and flip the matrix row to verified.

## Test plan

`tests/userland` gains a process-shared case: parent `shm_open`s + `ftruncate`s a
region, `mmap`s it `MAP_SHARED`, places a `pthread_mutex_t` (pshared attr) and a
shared counter in it, `fork`s; parent and child each lock/increment/unlock N
times; parent joins child and asserts the counter == 2N (no lost updates) and
that a deliberately-held mutex actually blocks the other side (timed lock).
Condvar: child waits on a pshared cond, parent signals it.  Runs under
`syscall-testing` on SMP.

## Risks and rollback

- Touches the kernel's most correctness/perf-sensitive SMP subsystem.  Mitigation:
  slice 1 is a zero-behavior-change refactor; every slice runs the full QEMU SMP
  regression before commit; private-path performance is unchanged (fast path
  untouched, resolution only on contended shared ops).
- If cross-process teardown or SMP races prove intractable, slices 1–4 can be
  reverted independently and the option taken fail-closed (the pragmatic
  alternative), with no residual half-state because the attr setters are the last
  thing flipped.
