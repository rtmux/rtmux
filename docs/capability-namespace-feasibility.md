# Capability-Aware Namespace Feasibility

This is the §7.1 entry artifact: inventory first, implementation later.

## Rule

Capability is authority.  Namespace is presentation.  File protocol is operation.

No RTMux document may claim capability security until unforgeability, delegation,
and revocation tests exist.

## Current Handle Inventory

| Handle | Width | Authority today | Capability-like? | First experiment |
| --- | --- | --- | --- | --- |
| pid | `pid_t` LP64 representation, generation plus slot | ambient lookup through servers/roottask/procfs | partial: generation tags reject reuse when callers keep the full pid | `/proc/<pid>/wait` process handles |
| tid | `pid_t`-shaped thread id | ambient process/thread lookup | no | stale-thread rejection after process-handle work |
| chid | `int` | receive endpoint inside owning process | partial: kernel object id, not delegated by namespace | named channel registry |
| coid | `int` | send endpoint to a channel/server | yes: existing file descriptors are coid-backed handles | fd-space handles and `openfd()` |
| fd | `int` libc index | per-process index over an open connection | yes at process boundary, not globally unforgeable | `/proc/<pid>/fd/<n>` checked reopen |
| rcvid | `int` | transient server reply right | yes but short-lived | keep internal to message transaction |
| interrupt id | `int` | interrupt detach/control token | partial | later, after revocation protocol |
| memory object | namespace object plus fd | mapping authority comes from opened file object | partial | `/dev/mem` and typed-memory metadata |
| device path | string in namespace | ambient if visible and permissions pass | no | namespace entry requires endpoint cap |
| private management calls | per-process `priv_caps` classes | private kernel sub-ops for the first management image | partial: caps, not the old broad flag, decide each op | split monolithic roottask into separate images |

Already-capability-like handles are `coid`, `fd`, `rcvid`, generation-tagged
process handles, and opened memory-object files.  Ambient authority remains in
path visibility and raw pid/tid lookup.

POSIX file descriptors stay small non-negative `int` values.  Endpoint
capabilities are the kernel-resident authority behind non-POSIX endpoint
delegation and namespace grants; libc fd slots remain process-local indexes
that refer to those authorities.  Do not encode generation bits directly into
POSIX fd values.

## Namespace Entry Shape

A future namespace entry records:

| Field | Type | Rule |
| --- | --- | --- |
| path | normalized absolute path string | presentation only; never sufficient authority |
| endpoint | `coid`-backed server endpoint | target for open/control messages |
| required capability | `uint64_t` generation-tagged cap id | 0 only for explicitly public entries |
| rights | `uint32_t` bitmask | `read`, `write`, `admin`, `mount` |
| namespace version | `uint64_t` | sampled by `open()` and checked before minting handle |

`open()` resolves the path, checks the required capability and rights against the
caller table, and returns a file-handle capability.  The libc fd remains only an
index in the caller's fd table.

The per-entry capability/rights columns are still future work, but the
`namespace version` row is already live: roottask maintains a global
`namespace_version` plus a per-node `entry_generation`, and the namespace open
path samples them to reject stale lookups and stale endpoint redirects with
`ESTALE`.
See `docs/vfs-contract.md`.

## Widths

Future cap-table and namespace-table indexes use fixed LP64-safe widths:

| Name | Width | Ceiling |
| --- | --- | --- |
| capability id | `uint64_t` | high 32 bits generation, low 32 bits slot |
| capability slot | `uint32_t` | checked `size_t` growth, hard table limit |
| namespace version | `uint64_t` | monotonic per namespace table |
| namespace entry id | `uint64_t` | high 32 bits generation, low 32 bits slot |
| rights | `uint32_t` | small mask; no pointer-sized casts |

No `int`-on-pointer, `uint32_t`-on-`pid_t`, or unchecked `size_t` growth is
allowed in the capability or namespace tables.

## First Experiments

| Experiment | Existing artifact | Feasibility question |
| --- | --- | --- |
| process handles | `docs/process-handle-design.md` | can a handle pin `(pid, generation)` and reject pid reuse? |
| fd-space handles | `docs/fd-introspection-design.md` | can fd reopen duplicate no more rights than the source? |
| named channel registry | `docs/channel-registry-design.md` | can the namespace service present endpoints without changing channel semantics? |

These experiments stay inside today's roottask/libc I/O-service surface.  They do not
require a kernel-wide capability table.

## Current Scaffold

The private `PRIV` syscall namespace now has coarse per-operation capability
classes in `kernel/include/private/priv-calls.h`, stored per process in
`tProcess.priv_caps`, and enforced by `kernel/core/priv.c` before the
handler dispatch.  Boot loads `/boot/roottask`.  The monolithic first image
still gets every class because it hosts the servers/roottask/coresrv/namesrv code
together; this is an authority split inside one process, not a separate `capd`
or revocation system.

`kernel/cap/table.c` provides the first kernel-resident capability table
primitive: flat 64-bit generation-tagged handles, per-slot `free` / `live` /
`revoking` / `dead` states, pin counts, generation-wrap slot retirement,
group-scoped revoke (a root and the handles delegated from it revoke
together; independent roots to the same object do not), and attenuated
delegation.  The table also exposes locked accounting snapshots for successful
alloc/delegate/transfer/revoke operations, stale revokes, denied lookups,
retired slots, and the last denied errno.  Host tests cover zero, malformed,
stale, high-slot,
overflow-sized, unknown-rights, delegated rights that cannot widen,
delegated-handle revoke, independent-root revoke isolation, pinned revoke,
32-bit and 64-bit revocation-group wrap, accounting snapshots, and
lookup/revoke contention cases.
Existing POSIX-visible `coid` / fd values are still current process-local vector
indexes, but every process now has a kernel endpoint cap table.  Endpoint cap
slots are generation-tagged 64-bit handles and carry a descriptor coid context
alongside the underlying `connect_t`; this keeps I/O-service OCB lookup
stable when multiple fds share one connection object.  `connect_t` still carries
the fast-path endpoint rights, and `msg_send*` / `msg_send_notification` reject
endpoints without `RTMUX_CAP_ENDPOINT_SEND`.

`_server_info` and `_msg_info` expose the current endpoint cap id and rights for
diagnostics/native code.  `connect_attach_cap()` mints a 64-bit endpoint handle
directly, and `connect_server_info()`, `connect_flags()`, and
`__connect_detach()` accept either raw coids or generation-tagged endpoint
caps.  `msg_send*_cap()` and `msg_send_notification_cap()` accept a 64-bit endpoint cap
directly; the kernel resolves it through the selected process cap table,
rewrites the internal send coid to the descriptor context, and rejects
revoked/stale handles with the same stable errno shape as a bad descriptor.
POSIX file descriptors remain process-local `int` values and do not encode
generation bits.

The first native file-message schema is now defined in `include/sys/iomsg.h`.
It reserves `_IOMGR_RTMUX_NATIVE` and `_IO_NATIVE_*` subtypes for attach, walk,
open, read, write, clunk, stat, and control-file writes.  The schema carries
caller credentials and explicit capability/right slots in every message; path
strings are presentation data, not authority.

Bounded capability transfer metadata is also defined for the native message
header.  `rtmux_cap_transfer()` copies up to four handles with rights
attenuation, rejects unknown flags and zero-right transfers, returns `E2BIG`
for oversize vectors, and prevalidates the vector under the table lock so a
rejected transfer allocates no partial handles.
The live buffered native IPC path now installs those transfer vectors into the
receiver process endpoint-cap table before delivery and rewrites ids to
receiver-local handles; buffered native replies install handles in the reverse
direction.  Descriptor context is preserved across delegation and transfer, so a
transferred endpoint cap continues to name the same server-side OCB identity.
`/cap/self` and `/dev/slog` guest coverage exercise valid transfer vectors,
while host coverage checks cross-table attenuation and object-wide revoke of
transferred endpoint handles.

Host stress now covers the cap-table revoke path with concurrent lookup/pin
users across root, delegated, and transferred handles.  It asserts new lookups
fail after revoke and repeated cycles leave no live, revoking, or pinned slots.

Guest stress now covers the current named-endpoint destruction path.  The VFS
suite races `name_detach()` against concurrent `name_open()`/`msg_send()`
clients and accepts only safe completion or endpoint failure, not a stale usable
endpoint after teardown.

Spawn now has the current hidden-until-commit invariant documented and covered.
The kernel creates children with `RTMUX_FLG_PROC_LOADING`; wait handling skips
loading/terming children, thread creation into another process is limited to
loading targets, and `process_startup(start=true)` is the commit point that
clears loading and enters user mode.  The VFS suite asserts that a missing
binary publishes neither pid nor wait fd, and that a committed child which takes
a user `SIGSEGV` fault reports `WIFSIGNALED` with
`WTERMSIG(status) == SIGSEGV`.

`/cap` is now mounted by roottask as a read-only diagnostic namespace.  The first
file, `/cap/self`, reports the caller pid, endpoint right names, native transfer
limit, and live per-process connection-handle counts split by fd-vector and
side-channel handles.  It also reports a self-scoped endpoint cap count, the
first endpoint cap id for diagnostics/tests, and the kernel endpoint cap-table
accounting snapshot for the caller: alloc/delegate/transfer/revoke counts,
stale revokes, denied lookups, retired slots, and last-denied errno.  The file
rejects write opens and redacts everything outside the caller's own namespace.
As the first native file-message endpoint, `/cap/self` also accepts
`_IOMGR_RTMUX_NATIVE` `STAT` and explicit-offset `READ` messages on an
already-open descriptor; existing POSIX-facing `stat(2)` and `read(2)` keep working.

The system logger is the first standalone server port.  `/dev/slog` keeps its
existing
circular-buffer `read`/`write` interface and additionally accepts native `STAT`
plus status `READ` messages on an already-open descriptor; native writes fail
closed with `EROFS`.

## Capability-Gated Global Channels

The global channel directory was the last name-only authority hole: a global
channel resolves by chid alone, so any process could connect to any registered
global channel by naming its integer chid, with no capability check. That gate
is now closed for channels that ask for it, while public channels stay open for
backward compatibility.

A channel created with RTMUX_FLG_CHF_CAP_GATED (only valid together with
RTMUX_FLG_CHF_GLOBAL) gains a nonzero required_cap on its global_channel_t. At
create time the kernel mints an endpoint capability over the channel object in
the creator's endpoint_caps table and hands the id back through the cap_out
argument slot. A required_cap of 0 marks an explicitly public channel, so every
existing global server keeps binding unchanged.

The connect path, on the global-chid branch, admits a connect to a gated
channel only when the caller holds a live capability naming that channel object.
Matching is by object identity, not by capability id: capabilities are
per-process-table with receiver-local ids, so an IPC-transferred capability
names the same object under a different id. rtmux_cap_has_object performs the
bounded object scan under the cap-table lock. The check is connect-time only and
never runs on the message send path, so the round trip is unchanged.

The registering server distributes the required capability with the existing IPC
capability transfer (delegating it to an authorized client) or a bootstrap
manifest grant; no new distribution mechanism is introduced. When a gated
channel is destroyed, the kernel revokes its gating capabilities across every
process table before the channel soul can be recycled, so a stale capability
over the old channel pointer cannot authorize a connect to a different object
that later reuses the soul.

The libc entry point is channel_create_cap(flags, cap_out). Coverage:
tests/host/test-cap-table.c exercises rtmux_cap_has_object (object match, rights
subset, wrong object, post-revoke miss), and the guest capgate suite in
tests/userland exercises the flag validation, a public channel connecting
without a capability, a capability holder connecting to a gated channel, and a
capability-less child process being refused with EPERM.

## Revocation Completeness Audit

An endpoint-capability slot stores the raw object pointer plus a generation. A
lookup checks the generation, then returns the pointer. A slot is invalidated
only when its generation is bumped by a revoke. The slot does not record the
object's own soul generation, so it cannot notice that the object memory (a
soul, from `object_alloc` / `object_free`) was freed and handed to a different
object. The hazard is therefore a soul freed and recycled while a live
capability still names it: the still-valid capability would resolve to the
recycled soul and authorize a different object. The invariant every cap-bearing
object type must hold is revoke-before-free, and where a capability can be
IPC-transferred to a process other than the object owner, the revoke must fan
out across every process table, not only the owner's.

| Object type | Names a cap? | Soul pool and free site | Revoke before free | Cross-process coverage |
| --- | --- | --- | --- | --- |
| `channel_t` (global gate) | yes, with `RTMUX_FLG_CHF_CAP_GATED` | `global_channel_souls` / `channel_souls`; freed in `kernel/ipc/channel.c` `_channel_destroy` | yes: `global_channel_revoke_gates` runs before the `object_free` | complete: it walks every process and revokes by object pointer |
| `connect_t` (endpoint) | yes, minted on demand, including a server minting a cap on a client cop at message receipt (`kernel/ipc/fastmsg.c`, `kernel/ipc/message-core.c`) | `connect_souls`; freed in `kernel/ipc/connect-core.c` `connect_detach` and `kernel/ipc/connect.c` `_connect_detach` | yes: `endpoint_cap_revoke_if_present` runs before every `object_free`, including the shared `connect_detach` soul-free path | complete: it walks every process and revokes by object pointer |
| interrupt id | no endpoint cap is minted for it | `interrupt_souls`, kept for the life of the attach, not recycled per request into a cap-referenced pool | not applicable | not applicable |
| memory object / MAP_PHYS BAR | no endpoint cap is minted for it; mapping authority is the opened file object (a coid), already covered under `connect_t` | direct kernel mapping, no per-object recycled soul pool | not applicable | not applicable |

Ordering proofs. For `channel_t`, `global_channel_revoke_gates` fans out across
`process_vector` and revokes by object before the channel soul is released, so a
transferred gating capability cannot survive to a recycled soul. For
`connect_t`, `endpoint_cap_revoke_if_present` fans out the same way, and it now
runs before every `object_free` of a connection soul: the shared `connect_detach`
path (channel-drain, dispatch, fast-message, and last-drop teardowns) and the two
direct server-connection frees in channel destroy. This closes the gap where
those paths previously revoked only the owner's table and left a server's
transferred capability, minted on the client `cop` at message receipt, live over
a recycled connection soul. The helper now guards on the connection pointer
alone, not on `cop->process`, so the DISCONNECT path that nulls `cop->process`
while keeping the soul alive still revokes by object pointer on the eventual
free. The interrupt id and MAP_PHYS memory object do not
mint an endpoint capability over a recycled soul, so they carry no soul-reuse
hazard today; if either gains a real capability later, it must adopt the same
fan-out-before-free rule.

## Deferred Implementation Work

The following remain TODOs, not claims:

- preserve POSIX-facing process/file behavior while internals move behind
  capabilities;
- add roottask bootstrap privilege drop;
- add latency/jitter budget gates for extra IPC hops.

Server crash and restart behavior is no longer deferred; it is defined and tested
(see the Server Crash and Restart section below).

## LKL Domain Authority

`docs/lkl-domain.md` is the §11 boundary for optional Linux driver reuse.
An LKL domain receives no ambient authority: MMIO ranges, IRQ notification
endpoints, DMA buffers, delegated block-image fds, and namespace binds are
explicit grants and absent by default.  The native RTMux IST remains the hard
IRQ owner; LKL receives only notifications or ordinary SRR messages after native
ack/mask/unmask policy runs.

## Server Crash and Restart

When an LKL domain server dies from a crash, panic, or fatal fault, its authority
does not outlive it, though the reclaim is split across the layers that granted
each resource rather than one kernel policy. The kernel drops the domain's IPC
endpoints and caps as the process tears down; roottask clears its hardware-resource
claims (the delegated MMIO and IRQ ranges) through the per-pid hwres release; and
process teardown frees the domain's memory and delegated block-image fds. The
namespace path vanishes with the dying server's dispatch ownership. A client
holding an old-generation control handle keeps no authority: once the dying
generation has finished tearing down, an operation on that handle fails closed
with an error rather than reaching a live domain.

Restart is supervised rather than a kernel policy. A domain server self-detaches
and publishes a namespace path, so it cannot be waited on with waitpid; a
supervisor (tests/userland/rt-latency/lkl-supervisor.c) watches that path instead.
The path vanishing is the abnormal-exit signal: the supervisor spawns a fresh
generation, which re-acquires its own grants and republishes the path, so a client
sees the path disappear as a transient error and then reappear. Because the
restart races the dying generation's cap and device reclaim, the supervisor
retries the spawn rather than guessing a settle delay; a generation that cannot
acquire its device exits, so a retry is safe and never leaves a duplicate owner.

This is exercised on the reference host by the lkl-fsd-restart and lkl-netd-restart regression
modes, which drive the full crash to reclaim to respawn to republish cycle, and by
the lkl-netd-stale mode, which proves old-generation handles fail closed. The
recovery latency is recorded, not promised, as restart_ms in docs/rt-budgets.md, a
coarse boot-dominated bound rather than a realtime metric.
