# RTMux VFS Transport / Namespace Contract

RTMux keeps file data transport separate from namespace policy.
The namespace borrows the Plan 9/Inferno file shape where it is useful, but the
transport is RTMux I/O-service messaging, not 9P/Styx.

## Transport

Transport is per connection.  `read`, `write`, `msg_read`, and `msg_write` move
bytes over an existing connection and must not redo path lookup.  Transport
paths may block on the target I/O service, but they must not mutate mount
tables or permissions policy.

## Namespace Policy

Path lookup, mount, permissions, procfs control files, and process-handle files
belong to servers/roottask/path.  These paths may allocate roottask-owned policy
objects and consult credentials.  They are the only place that should decide
which server receives an open or control request.

## Namespace Staleness

Path lookup and the bind that produces a descriptor are not atomic, so namespace
churn between them must never yield a descriptor bound to the wrong object.  Two
monotonic counters enforce this: a global `namespace_version` (bumped on object
attach/detach) and a per-node `entry_generation` (bumped on that node's object
attach/detach).  `open()`
samples `namespace_version` before lookup and rejects the result with `ESTALE`
if the namespace keeps changing before a result binds; it also fails stale named
endpoint redirects and directory OCB binds with `ESTALE` when the node's
`entry_generation` moves between snapshot and bind.  `/sys/namespace-stats` exposes
the current `namespace_version`.  Userspace must treat `ESTALE` from `open()` as
retryable, not fatal.

## Classification

| Operation | Class | Owner |
| --- | --- | --- |
| `open`, `openat`, `creat` | namespace policy plus connection creation | servers/roottask/path, then target I/O service |
| `read`, `pread`, `write`, `pwrite` | transport | target I/O service over existing connection |
| `fstat` and internal chmod/chown messages | transport or per-connection policy | target I/O service |
| `stat`, `lstat`, `readlink`, `fstatat` | namespace policy plus target query | servers/roottask/path resolves, target I/O service answers |
| `mount`, `umount` | namespace policy | servers/roottask/path |
| `/proc/<pid>/fd` reopen | namespace policy with handle validation | servers/roottask/procfs, then target I/O service via `_IO_OPENFD` |
| `openfd()` direct call | per-connection duplication | libc + target I/O service (no namespace lookup) |

Tests should be able to exercise a transport-only path with a preexisting
connection and no path lookup.  Namespace tests should be able to use adapters
for kernel calls and message transport so namespace policy can run on the host.

## Native File-Message Subset

Native I/O services use `_IO_MSG` with `mgrid == _IOMGR_RTMUX_NATIVE`.
The subtype is one of `_IO_NATIVE_ATTACH`, `_IO_NATIVE_WALK`,
`_IO_NATIVE_OPEN`, `_IO_NATIVE_READ`, `_IO_NATIVE_WRITE`,
`_IO_NATIVE_CLUNK`, `_IO_NATIVE_STAT`, or `_IO_NATIVE_CTL_WRITE`.
This is the RTMux file protocol target for new capability-aware servers; the
base `_IO_CONNECT`, `_IO_READ`, `_IO_WRITE`, `_IO_STAT`, and `_IO_DEVCTL`
messages remain valid ABI.

Each native message starts with `struct _io_native_hdr` from
`include/sys/iomsg.h`.  The header carries the wire version, byte count,
request id, caller credential snapshot, three explicit capability slots
(root, object, and I/O), and up to four bounded capability transfers.  The
receiving server must treat path text as presentation only and must authorize
against the carried capabilities, rights, and kernel-provided sender
information.

Capability transfers use `struct _io_native_cap_transfer`: a generation-tagged
handle plus attenuated rights.  Unknown transfer flags are invalid, transfers
with zero rights are invalid, and more than `_IO_NATIVE_CAP_TRANSFER_MAX`
entries must fail early with `E2BIG`.

| Message | Shape | POSIX result rule |
| --- | --- | --- |
| `ATTACH` | attach to a server root or named export | success returns a root fid/object cap; failure replies with errno |
| `WALK` | resolve a relative path from one fid to another fid | path lookup races are rejected with errno rather than silently retargeted |
| `OPEN` | bind open flags and mode to an existing fid | checks requested rights; returns an opened I/O cap or errno |
| `READ` | read at an explicit offset and max byte count | returns byte count; zero means EOF; short reads are valid |
| `WRITE` | write bytes at an explicit offset | returns committed byte count; short writes are valid |
| `CLUNK` | release a fid/open object | close-style errors are returned as errno; authority is not widened |
| `STAT` | return `struct stat` metadata for a fid | metadata must describe the authorized object, not a later path lookup |
| `CTL_WRITE` | write textual or binary data to `/ctl`-style files | command-specific errno; no bespoke ioctl authority outside the caps |

Servers that implement the subset should expose diagnostics through ordinary
files such as `/ctl`, `/stats`, or `/status`.  Control-file writes use
`_IO_NATIVE_CTL_WRITE`; reads use `_IO_NATIVE_READ`.

Current namespace state:

| Namespace | Native subset state |
| --- | --- |
| `/proc` | roottask procfs messages only |
| `/dev/slog` | log `read`/`write` plus native `STAT` and status `READ` |
| `/cap` | read-only capfs scaffold plus native `STAT` and explicit-offset `READ` |
| `/fs/root` | absent unless an `lkl-fsd` namespace bind is delegated |
| `/net` | absent unless an `lkl-netd` namespace bind is delegated |

The current sandbox conformance tests exercise the global namespace behavior
available before `namesrv`: minimal allowed opens (`/dev/zero`), buildbox-style
read inputs (`/bin/true` and `/sys/namespace-stats`), network-denied absence
(`/net/tcp/clone` and `/fs/root` return `ENOENT`), visible read-only denial
(`/cap/self` write opens return `EROFS`), and hidden named endpoints remaining unreachable
through canonical lookup, cwd-relative lookup, symlink traversal, stale
`/proc/self/fd` reopen, and `stat()`.  Path resolve/chdir/chroot dispatch now
enters the `namesrv` extraction boundary before touching the current
namespace tables.  Private per-process namespace storage is still roottask-backed,
but the conformance tests pin the POSIX errno and escape behavior that the
split-server implementation must preserve.

For buffered native messages, the kernel installs nonzero transfer vectors into
the receiver endpoint-cap table before the server handles the request, and does
the reverse for buffered native replies, but only for a client that opted in
with `thread_ctl(RTMUX_TCTL_NATIVE_REPLY, (void *) 1)` around its request (the
kernel never infers a native reply from its bytes, since reply data such as file
content is attacker-controlled).  Oversize vectors fail early with
`E2BIG`.  `/cap/self` exposes self-scoped capability metadata, live
fd/side-channel handle counts, the caller's first endpoint cap id for
diagnostics/tests, and the kernel endpoint cap-table accounting snapshot.
Endpoint caps preserve the descriptor coid context needed by I/O-service
OCB lookup, so a native request may be sent through `msg_send_cap()` without
falling back to the process-local integer fd.  `connect_attach_cap()` returns
the same generation-tagged handle form, and endpoint lifecycle helpers accept
those caps for info, flags, and detach operations.  Closing or detaching the
descriptor revokes that descriptor-context cap; stale cap sends fail with
`EBADF`.

## Capability / Namespace Regression Gate

Run `make check-cap-namespace` from the repo root before landing §7 capability
or namespace patches.  The gate runs host capability tests, userland test
builds, POSIX matrix drift checks, an interactive QEMU smoke boot, and the
guest syscall suite.  Realtime probes remain under `make check-rt` until §7.6
adds capability-specific latency budgets.
