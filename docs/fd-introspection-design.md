# RTMux FD Introspection And Transfer Design

FD introspection starts with reopening descriptors through procfs.  Explicit
cross-process FD transfer comes later.

## Initial Interface

`/proc/<pid>/fd/<n>` names an open descriptor in the target process.  Opening
that procfs node calls a checked `openfd()` path in roottask:

- validate caller credentials;
- resolve `(pid, generation)` so PID reuse is rejected;
- validate the target descriptor is still open;
- duplicate the connection with permissions no broader than the source.

Close semantics stay normal: closing the reopened descriptor only drops the new
connection unless explicit transfer/revoke policy is added later.  Once the
source descriptor is closed, immediate `/proc/<pid>/fd/<n>` reopen attempts
must fail instead of reaching a stale connection.  The current user-visible fd
model is still integer-indexed, so a later open may legitimately reuse the same
number until fd capabilities grow per-entry generations.

## Current Scaffold

`/proc/<pid>/fd` exists as a procfs directory, is advertised from
`/proc/<pid>`, and lists normal descriptor numbers from the target process's fd
connection vector.  libc recognizes `/proc/<pid>/fd/<n>` and reopens it through
the same `_IO_OPENFD` duplicate path used by `openfd()`, so I/O services
keep their normal permission checks.  Servers that do not implement
`_IO_OPENFD` reject the reopen; `/dev/null` and `/dev/zero` provide simple
checked fixtures for the regression suite.

Since Q22 an I/O service admits an openfd only for the sender's own
connection, roottask, or a message carrying a verifiable key (see
`docs/capability-abi.md`, Handle Duplication Keys). libc reopens `/proc/<self>/fd/N`
itself (the sender's own connection). For another pid it sends the plain path
open to roottask: `procfs_fd_reopen` in `servers/roottask/path/procfs.c` admits
only a root-euid caller on the local node (`EACCES` for anyone else, before the
target's existence is revealed; `EBADF` if the target fd is closed), and replies
with a message link carrying the `_IO_OPENFD` and a key minted for the opener's
thread over exactly those bytes. The I/O service still applies its own checks,
so the reopen is no broader than the source. `capgate` covers the root success
(twice in turn), the closed fd, and the non-root refusal.

## Later Transfer

Cross-process delegation should use explicit grant/revoke lists, not ambient
PID authority.  A grant names the source process generation, descriptor, target
process generation, allowed rights, and optional expiry.

The module seam is procfs/roottask.  I/O services continue to see normal
connections; they do not learn procfs lookup rules.
