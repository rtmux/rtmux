# RTMux Capability ABI Contract

This document is the append-only contract for kernel authority visible to libc,
roottask, servers, and tests. It is not a compatibility layer: POSIX behavior is
implemented above this ABI in libc and user-mode services.

## Core Rules

Append-only rule: keep existing kernel-call numbers, object tags, rights bits,
and errno meanings stable. New authority-bearing calls must add a row to the
kernel-call authority ledger before they land.

Capability is authority; namespace is presentation. A path, pid, uid, gid,
global chid, or guessed integer can select a presentation object, but it cannot
grant authority by itself. No kernel syscall grants authority through a name,
pid, uid, global id, or guessed integer.

The current object classes are `RTMUX_CAP_TYPE_ENDPOINT`,
`RTMUX_CAP_TYPE_PROCESS`, and `RTMUX_CAP_TYPE_THREAD`. Endpoint capabilities
carry `RTMUX_CAP_ENDPOINT_SEND`, `RTMUX_CAP_ENDPOINT_RECEIVE`, and
`RTMUX_CAP_ENDPOINT_GRANT`. Process capabilities carry
`RTMUX_CAP_PROCESS_INSPECT`, `RTMUX_CAP_PROCESS_MAKE_THREAD`,
`RTMUX_CAP_PROCESS_CONTROL`, and `RTMUX_CAP_PROCESS_KILL`. Memory-map rights use
`RTMUX_CAP_MEM_MAP_READ`, `RTMUX_CAP_MEM_MAP_WRITE`, and
`RTMUX_CAP_MEM_MAP_EXEC`; W|X mappings are rejected at the VM boundary.

Capability ids are `uint64_t`, with the generation in the high 32 bits and the
slot in the low 32 bits. A stale generation returns `ESTALE` in the cap table and
the public endpoint-cap send path reports stale or forged handles as `EBADF`.
Rights checks are type-local: typed lookups reject the wrong object class before
testing the rights mask.

Delegation is monotonic. `rtmux_cap_delegate()` and `rtmux_cap_transfer()` may
only reduce rights. Transfer metadata is bounded by `RTMUX_CAP_TRANSFER_MAX`;
oversize native transfer vectors fail `E2BIG` and invalid vectors allocate no
partial handles.

Revocation is group-scoped and object-safe. Revoking a root handle revokes the
handles delegated or transferred from it; independent roots to the same object
remain independent. Every cap-bearing recycled object must revoke before free,
and cross-process endpoint or global-channel capabilities must fan out revoke by
object pointer before the soul can be reused.

POSIX fds remain small process-local `int` indexes. They may refer to
capability-backed connections, but the fd value is not itself a global
generation-tagged capability.

## Error Shape

| Condition | Error |
| --- | --- |
| Malformed, zero, wrong-generation, or wrong-slot cap-table handle | `ESTALE` |
| Public endpoint-cap operation on a stale or forged endpoint handle | `EBADF` |
| Requested rights not present | `ESTALE` in the cap table, `EBADF` through fd/cap send |
| Zero-right allocation, delegation, transfer, or lookup | `EINVAL` |
| Oversize transfer metadata | `E2BIG` |
| Missing private or typed-process authority | `EPERM` |
| Unsupported reserved syscall slot | `ENOSYS`; no authority |

## Memory Accounting

Capability tables use fixed slot arrays owned by the process or subsystem that
created them. Slot growth is checked against `uint32_t` slot ids; generation wrap
retires the slot instead of silently reusing a handle. `/cap/self` exposes
self-scoped allocation, delegation, transfer, revoke, denied-lookup, and retired
slot counters for regression evidence without exposing another process's table.

## Self-Only Exceptions

The kernel may expose self-only calls that do not name another object and do not
grant authority: current-thread scheduling/yield, current-thread timeout/wait,
caller-local TLS/cpupage setup, and current-process diagnostics. Cross-process
or cross-object variants must use a held capability or fail closed.

## Kernel-Call Authority Ledger

| Kernel call | Authority class | Rule |
| --- | --- | --- |
| `__KSYS_NOP` | self only | Caller-local observation or yield; no object lookup and no authority amplification. |
| `__KSYS_TRACE_EVENT` | diagnostic | Diagnostic surface; production grants come from bootstrap/private capability policy. |
| `__KSYS_PRIV` | private capability class | Sub-ID dispatch is gated by `kernel/include/private/priv-calls.h` and caller `priv_caps`. The `DEBUG_INJECT` sub-ID (class `RTMUX_PRIV_CAP_INJECT`, bit 14) exists only with `CONFIG_DEBUG_INJECT=y`: it can stall or hang another hart, hold the console lock on one (`spin-console`), and fail one allocation, so only `/bin/inject` holds it (a separate toolbox copy with its own boot-manifest grant). With the option off the sub-ID is unbound and a privileged caller gets `ENOSYS`; a caller without the cap gets `EPERM`. |
| `__KSYS_DBGPRINT` | diagnostic | Debug surface; production grants come from bootstrap/private capability policy. |
| `__KSYS_SPARE2` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_SPARE3` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_SPARE4` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_SYS_CPUPAGE_GET` | self only | Reads caller-visible syspage/cpupage values only. |
| `__KSYS_SYS_CPUPAGE_SET` | self only | Sets caller process-local storage only; no object lookup. |
| `__KSYS_SYS_SPARE1` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_MSG_CURRENT` | self only | Reports caller IPC state only. |
| `__KSYS_MSG_SENDV` | endpoint capability | Requires endpoint send authority through a coid or endpoint cap. |
| `__KSYS_MSG_SENDVNC` | endpoint capability | Non-cancelable variant of send; requires the same endpoint authority. |
| `__KSYS_MSG_ERROR` | live transaction | Operates on a live server rcvid/reply transaction. |
| `__KSYS_MSG_RECEIVEV` | held channel | Receives only on a caller-held channel. |
| `__KSYS_MSG_REPLYV` | live transaction | Replies only to a live rcvid owned by the server transaction. |
| `__KSYS_MSG_READV` | live transaction | Reads only from a live client/server transaction. |
| `__KSYS_MSG_WRITEV` | live transaction | Writes only to a live client/server transaction. |
| `__KSYS_MSG_READWRITEV` | closed slot | Removed slot; returns `ENOSYS` and grants no authority. |
| `__KSYS_MSG_INFO` | live transaction | Reports only live transaction metadata. |
| `__KSYS_MSG_SEND_NOTIFICATION` | endpoint capability | Requires endpoint send authority. |
| `__KSYS_MSG_DELIVER_EVENT` | live event target | Delivers only through validated event/notification state. |
| `__KSYS_MSG_KEYDATA` | live transaction | Operates only on live message key data. CALCULATE needs `RTMUX_PRIV_CAP_IPC` (`EPERM` otherwise); VERIFY needs no cap, since it only checks a key against a client thread on one of the caller's own connections. |
| `__KSYS_MSG_READIOV` | live transaction | Reads iov metadata only for a live transaction. |
| `__KSYS_MSG_RECEIVENOTIFICATIONV` | held channel | Receives notifications only on a caller-held channel. |
| `__KSYS_MSG_VERIFY_EVENT` | live event target | Verifies caller-supplied event metadata; grants no new authority. |
| `__KSYS_CHANNEL_CREATE` | caller-owned channel | Creates only a caller-owned channel; gated globals mint a caller-held gate cap. |
| `__KSYS_CHANNEL_DESTROY` | caller-owned channel | Destroys only the caller-owned channel and revokes gate caps before free. |
| `__KSYS_CHANNEL_SPARE0` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_CHANNEL_SPARE1` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_CONNECT_ATTACH` | endpoint or object gate | Attaches only to public endpoints or gated endpoints authorized by a held cap. |
| `__KSYS_CONNECT_DETACH` | held endpoint | Detaches caller-held coid or endpoint cap and revokes endpoint caps. |
| `__KSYS_CONNECT_SERVER_INFO` | held endpoint or process capability | Reports caller-held endpoint metadata; cross-process table walks require private process authority. |
| `__KSYS_CONNECT_CLIENT_INFO` | live transaction | Reports client info only for a live server transaction. |
| `__KSYS_CONNECT_FLAGS` | held endpoint | Changes flags/rights only on a caller-held endpoint. |
| `__KSYS_CONNECT_SPARE1` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_CONNECT_SPARE2` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_THREAD_CREATE` | self or process cap | Self create is local; cross-process create requires private class or `RTMUX_CAP_PROCESS_MAKE_THREAD`. |
| `__KSYS_THREAD_DESTROY` | process-local thread | Destroys only a thread in the caller process. |
| `__KSYS_THREAD_DESTROYALL` | self process | Marks only caller process threads for teardown. |
| `__KSYS_THREAD_DETACH` | process-local thread | Detaches only a thread in the caller process. |
| `__KSYS_THREAD_JOIN` | process-local thread | Joins only a thread in the caller process. |
| `__KSYS_THREAD_CANCEL` | process-local thread | Cancels only a thread in the caller process. |
| `__KSYS_THREAD_CTL` | process-local thread | Controls only caller-process threads. `RTMUX_TCTL_INTERRUPT` (data = 1-based tid) breaks a sibling's interruptible blocked call with `EINTR`; same-process only, `EAGAIN` unless the target waits in SEND, REPLY, RECEIVE, CONDVAR, SEM, NANOSLEEP or JOIN (never a mutex wait), `ESRCH` for an unknown tid. |
| `__KSYS_THREAD_SPARE1` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_THREAD_SPARE2` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_INTERRUPT_ATTACH_THREAD` | interrupt capability class | Requires IRQ private capability class. |
| `__KSYS_INTERRUPT_ATTACH_EVENT` | interrupt capability class | Requires IRQ private capability class. |
| `__KSYS_INTERRUPT_DETACH` | held interrupt id | Detaches only caller-held interrupt state. |
| `__KSYS_INTERRUPT_WAIT` | held interrupt id | Waits only on caller-held interrupt state. |
| `__KSYS_INTERRUPT_MASK` | interrupt capability class | Requires IRQ private capability class. |
| `__KSYS_INTERRUPT_UNMASK` | interrupt capability class | Requires IRQ private capability class. |
| `__KSYS_INTERRUPT_SPARE1` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_INTERRUPT_SPARE2` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_INTERRUPT_SPARE3` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_INTERRUPT_SPARE4` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_CLOCK_TIME` | public read or time capability | Standard clock reads are public; runtime clock ids are process-local; setting time is private capability-gated. |
| `__KSYS_CLOCK_SPARE0` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_CLOCK_PERIOD` | time capability class | Clock period control is private capability-gated. |
| `__KSYS_CLOCK_ID` | process-local clock id | Maps only the caller process or one of its threads. |
| `__KSYS_CLOCK_SPARE2` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_TIMER_CREATE` | caller-owned timer or endpoint capability | Creates caller-owned timer ids; notification timers require endpoint send authority. |
| `__KSYS_TIMER_DESTROY` | caller-owned timer | Destroys only caller-owned timer ids. |
| `__KSYS_TIMER_SETTIME` | caller-owned timer | Arms only caller-owned timer ids. |
| `__KSYS_TIMER_INFO` | caller-owned timer | Reports only caller-owned timer ids. |
| `__KSYS_TIMER_SPARE0` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_TIMER_TIMEOUT` | self only | Blocks only the calling thread. |
| `__KSYS_TIMER_SPARE1` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_TIMER_SPARE2` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_SYNC_CREATE` | caller memory object | Initializes caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_DESTROY` | caller memory object | Destroys caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_MUTEX_LOCK` | caller memory object | Blocks on caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_MUTEX_UNLOCK` | caller memory object | Unlocks caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_CONDVAR_WAIT` | caller memory object | Waits on caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_CONDVAR_SIGNAL` | caller memory object | Signals caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_SEM_POST` | caller memory object | Posts caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_SEM_WAIT` | caller memory object | Waits on caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_CTL` | caller memory object | Controls caller-provided sync state; no global lookup. |
| `__KSYS_SYNC_MUTEX_REVIVE` | caller memory object | Revives caller-provided sync state; no global lookup. |
| `__KSYS_SCHED_GET` | process-local scheduler state | Reports only caller-process scheduler state. |
| `__KSYS_SCHED_SET` | process-local or scheduler capability | Caller-process changes are local; priorities at or above `priv_prio` require `RTMUX_PRIV_CAP_SCHED`. |
| `__KSYS_SCHED_YIELD` | self only | Yields the calling thread only. |
| `__KSYS_SCHED_INFO` | process-local metadata | Reports caller-process scheduler metadata without granting authority. |
| `__KSYS_SCHED_SPARE0` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE0` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE1` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE2` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE3` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE4` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE5` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE6` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_NET_SPARE7` | closed slot | Reserved; returns `ENOSYS` and grants no authority. |
| `__KSYS_BAD` | closed slot | Bad-call sentinel; returns `ENOSYS` and grants no authority. |

## Handle Duplication Keys (`_IO_DUP` / `_IO_OPENFD`)

An I/O service (`_ioserv_access`) admits a dup or openfd of a handle only when
the sender names its own connection (same scoid), the sender is roottask (the
session plumbing and procfs), or the message carries a key that `msg_key_data`
VERIFY accepts for the sender's thread over the exact bytes preceding the key.
Equal uids, a root sender, or a guessed scoid grant nothing.

- **Key construction.** CALCULATE (needs `RTMUX_PRIV_CAP_IPC`) draws a fresh
  64-bit secret for the destination thread, stores it in the thread, and returns
  the low 32 bits of SipHash-2-4 of the message under that secret. The caller's
  seed argument is ignored. A thread with no secret never verifies. The wire key
  is 32 bits; the secret is not recoverable from samples (the earlier FNV-style
  hash was a bijection of its seed and was replaced; `capgate` keeps a model of
  it and shows a forgery from a minted pair is refused).
- **Entropy.** The kernel has no hardware RNG. Secrets come from a SipHash
  ratchet pool stirred by the cycle counter, the time base, the cpu number, the
  caller and a counter on every keydata call. That is timing jitter, not a
  CSPRNG: an attacker who can reproduce the exact cycle count of a mint could
  narrow the search. A hardware entropy source (virtio-rng or a boot seed)
  would remove this caveat. Nothing in the tree supplies one today: the boot
  path reads no FDT `/chosen/rng-seed`, no SBI call returns random bytes, and
  no Zkr `seed` CSR is used. When one exists, mix 64 bits of it into the pool
  once at boot (in `keydata_stir`'s state, `kernel/ipc/message-core.c`) and
  keep `KEYDATA_MAX_FAILS`: the bound limits online guessing whatever the seed
  quality. Closed as "kept, with the bound" until then.
- **VERIFY is an oracle.** It needs no cap and is only limited to client
  threads on the caller's own connections, so a caller can test guesses against
  the key of its own thread (send to its own channel, then VERIFY). Without a
  limit that is a 2^32 search (hours at most). Each minted secret is therefore
  retired after `KEYDATA_MAX_FAILS` (8) wrong VERIFYs, so a guess has about
  8 / 2^32 chance per mint, and every new mint costs a `/dev/tty` open or a spawn.
  A legitimate server sees EACCES on a stale key, never a retry window.
- **Spawn.** The loader thread asks `_PROC_SPAWN_FD` for a key per fd; the proc
  service serves only a child's own loader about its parent, only while fd
  inheritance is open, one fd per call, and only an fd the parent's spawn
  request offered (an explicit list entry other than `SPAWN_FDCLOSED`, or in scan
  mode a live non-close-on-exec fd at its own number). The thread key is
  scrambled at `_PROC_SPAWN_DONE`, `_PROC_SPAWN_EXEC`, and when the loader
  abandons loading. Untrusted code first runs after `process_startup(start)` in
  `_PROC_SPAWN_DONE` (spawn) or after the `process_exec` swap in
  `_PROC_SPAWN_EXEC` (exec); fd inheritance is closed before either.
- **`/dev/tty`.** The path service mints the opener's key; the open fails if the
  mint fails. A second mint for the same thread replaces the first key, but a
  thread cannot start a second open before the first link is followed (the open
  is one synchronous libc call), so the only way to lose a key is a signal
  handler that opens `/dev/tty` between the reply and the follow. That fails
  closed with `EACCES`; it grants nothing. Closed as by design.
- **Foreign-pid openfd.** An `_IO_OPENFD` naming another process's connection
  without a key returns `EACCES`; there is no uid or root fallback in the I/O
  service. libc reopens only its own `/proc/<self>/fd/N` by sending
  `_IO_OPENFD` itself. For another pid it sends the plain path open to roottask
  (`servers/roottask/path/procfs.c`, `procfs_fd_reopen`), which admits only a
  root-euid caller on the local node (`EACCES` otherwise, `EREMOTE` for a remote
  caller), checks the target fd is open (`EBADF`), and replies with a message
  link carrying the `_IO_OPENFD` plus a key minted for the opener's thread over
  those bytes. The service still enforces its own checks. Root foreign reopen
  therefore works again, but through a key the policy server issued, not an
  ambient service-side rule.
- **Remote node.** The remote-node fd path (a remote caller reopening, or the
  net-manager `msg_info` change) has no in-tree remote node to run against.
  The path service refuses non-local callers (`EREMOTE`) in both the `/dev/tty`
  and procfs mints, and `_ioserv_access` honors the roottask exemption only for
  a local sender, so nothing remote is admitted. Closed with that reason; a
  remote test needs a second RTMux node, which the QEMU gates do not provide.

## Native-Cap Replies

A reply is parsed as a `struct _io_native_hdr` and its capability transfers are
installed only when the client thread opted in with
`thread_ctl(RTMUX_TCTL_NATIVE_REPLY, 1)` before its send; otherwise the reply
bytes are opaque data (a read reply can spell a native header). The opt-in is
per thread, set by the client and read by the replier after the rendezvous. A
client that needs a cap reply must opt in around exactly that request and must
not ask for the cap if the opt-in is refused.

Reply-cap users in the tree: `rtmux_spawn_cap` (`SPAWN_PROC_CAP`, answered by
the proc service in `servers/roottask/proc/spawn.c`) is the only one; it opts in
and fails the call, requesting no cap, if `thread_ctl` refuses. Every other
`_IOMSG_RTMUX_NATIVE` user (`lkl-fsd`, `capfs`, `logger`, `ipc-roundtrip`)
carries capabilities in the request, which needs no opt-in, and answers with
plain data or a status, so none of them uses reply caps. A new server that
replies with transfers must document its client opt-in here.

## Evidence Gate

`make check-capability-abi` runs this document checker and then
`make check-cap-namespace`. The latter covers the live unforgeability,
delegation, transfer, attenuation, revoke, stale-handle, bad-rights,
cross-process thread authority, gated-global-channel, native transfer-bound, and
W|X rejection regressions.
