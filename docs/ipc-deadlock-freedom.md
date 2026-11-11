# SRR Deadlock Freedom

Synchronous Send/Receive/Reply (SRR) can deadlock when servers block sending
to each other in a cycle. This note states the wait-for relation, the
send-rank invariant that rules cycles out, and what is and is not checked.
`docs/smp-contract.md`, `tests/smt/srr-pi-safety.smt2`, and
`tests/spin/pi-walk-terminate.pml` cover priority inheritance and rendezvous
progress, not the wait-for graph; this note covers the graph.

Gate: `make check-ipc-deadlock` (part of `make check-formal`).
Status: `Tested` (`docs/formal-claims.md`, row `ipc-deadlock-freedom`).

## Wait-for relation

Let `A -> B` be a wait-for edge when a thread of `A` can block in
`MsgSendv` (any `msg_send`, `msg_sendv`, `msg_sendnc`, `msg_sendvnc`, `_r`
variant, or an `open()`/`read()`/`write()` that reaches one in libc) on a
channel served by `B`, and `A` cannot make progress until `B` replies.

Scope: blocking server-to-server sends only.

- Reply does not block. `MsgReply` completes the rendezvous the sender is
  already waiting on; the sender is parked, so the reply never waits.
- Notifications (`msg_send_notification*`, pulses) do not block; they are
  queued and are not edges.
- Timeouts and kernel-initiated unblocks (signals, `ETIMEDOUT`) can only
  remove edges, so they never add a deadlock the table does not already allow.

## Send-rank invariant

Every node (a server, or a role of a server) has an integer rank in
`docs/ipc-send-ranks.tsv`. Edges come from two places: `ipc-send:` annotations
at the call sites (below), and the table's `sends_to` column for edges with no
call site in the tree. The invariant:

> A blocking send goes only to a node of strictly higher rank.

For the edges in the table and the annotations,
wait-for paths therefore strictly ascend, so no
cycle exists, and a path cannot be longer than the number of distinct ranks. A
request is blocked only behind a request of a higher rank; the highest rank
(`roottask-core`) sends nothing, so some chain always makes progress. These are
statements about the edge set; the gate ties that set to the tree only as
described under Gap. The longest chain in the shipped
graph (reported by the gate) bounds the priority-inheritance chain: a thread
inheriting along a wait chain walks at most that many hops, well under the
`SYNC_PI_MAX_DEPTH` (16) walk bound modeled by `tests/smt/srr-pi-safety.smt2`
(`make check-srr-pi-smt`) and `tests/spin/pi-walk-terminate.pml`; the measured
boost cost is the `pi_boost` row in `docs/rt-budgets.md`. The checker fails if
the chain depth ever exceeds 16 (a regression guard: ranks already bound the
depth by the node count). Acyclicity and chain length say nothing about elapsed
time: a server can still wait to be scheduled or to reply, so a worst-case
blocking-time claim needs separate service-time, scheduling-interference, and
timeout bounds (see `docs/rt-budgets.md`); this note makes no such claim.

### Roles

A node is a server or a role (a thread class with its own blocking behavior)
of one. `roottask` is split because it is both the leaf every server calls
and a client of file servers:

- `roottask-core`: path resolution, spawn/exit, hardware-resource claims,
  anonymous VM. Sends nothing blocking to other servers.
- `roottask-fsio`: file-backed work done on behalf of a client (loader reads,
  file-backed `mmap`, `io_openfd`, sync). Blocks on whichever server owns the
  fd: the file servers (`romfs`, `lkl-fsd`), `dev-nvme`, `pipe`, and on
  `roottask-core` (loader self-sends to `ROOTTASK_COID`, which must run on a
  worker distinct from the thread serving it). Every such target ranks above it.
- `logger` is split likewise: `logger-ring` serves `/dev/slog`; `logger-drain`
  is the thread that reads it and writes the log file.
- `virtio-mmio-register` is the short-lived `virtio-mmio --register-boot`
  client process, distinct from the `virtio-mmio` service.

File servers may call only `roottask-core` operations. A file server issuing a
file-backed request that lands on `roottask-fsio` (for example file-backed
`mmap` of its own files) would create `romfs -> roottask-fsio -> romfs`; the
rank check forbids it.

The split is enforced per call site, not per message class. Every send site
under `servers/roottask/` must be annotated with the role it runs as. A site
labeled `roottask-core` fails the rank check (the top rank has no higher
target), so every `roottask` send site must be labeled `roottask-fsio`; a file
server's site that names `roottask-fsio` as its target fails the same way. What
cannot be enforced is which handler a message reaches: `ROOTTASK_COID` and
`VM_COID` are one endpoint, and whether a given message type is served by a
core or an fsio thread is decided at run time by the dispatcher, so an
annotation stating `roottask-core` for a request that the dispatcher would hand
to an fsio-class handler is not detectable from the call site. The kernel has
no per-class endpoint to check against (see Gap).

## Security invariant

An edge exists only through a held connection: the sender's endpoint capability
must carry `RTMUX_CAP_ENDPOINT_SEND` (checked in `kernel/ipc/fastmsg.c`,
`rtmux_cap_lookup_pin_context(..., RTMUX_CAP_ENDPOINT_SEND, ...)` and the
`cop->rights` test), and the receiver sees a kernel-derived sender identity
(`rep->pid = cop->process->pid`, `fastmsg_fill_rcvinfo`), never a
sender-supplied one. So a process cannot add an edge by naming a server, and
cannot impersonate a higher-rank sender to hide a back-edge. The edge set is
the set of connections the bootstrap manifest and server code actually hold;
`docs/capability-law.md` covers how connections are granted.

## Call-site annotations

Every blocking send site under `servers/` and `userland/` (comments skipped;
`userland/lib` and `userland/include` are libc wrappers and skipped) must be
covered by a comment:

    /* ipc-send: SRC[,SRC] -> DST[,DST] */   one per call site, on its line or within the 4
                                             lines above it

Every call of `open` or a relative that resolves a path (`openat`, `creat`,
`fopen`, `freopen`, `opendir`, and the `64` forms) needs its own `ipc-send:`
like a `msg_send*` or `MsgSend*` call; there is no file-wide cover and the old
`ipc-send-file:` form is rejected. `msg_send_notification*` is non-blocking and
exempt. A member call (`ops->open(`, `s.open(`) and a field declarator
(`io_open_t open;`) are not the libc function. Every call on a line is scanned,
so two calls on one line need two annotations on separate lines. An `open()`
resolves through roottask and then reaches whichever server owns the path.

Targets are derived where the C code allows. The gate collects every
`ioserv_attach(dpp, attr, "/path", ...)` whose path is a string literal; the
file that attaches a path is its owner (the highest-rank node covering the
file, so `servers/roottask` is `roottask-core`: `ROOTTASK_COID` and `VM_COID`
are one endpoint). For a call whose path argument is a literal (adjacent
literals concatenate), the targets are `roottask-core` plus the owner of the
longest attached prefix, and the annotation must name all of them or the gate
rejects it. Extra targets are accepted and rank-checked: naming more than the
derived set is conservative, naming fewer is the unsound direction. A path under
no literal attach (a root-filesystem path, whose mount point is chosen at run
time) and every path variable remain reviewer assertions and name a broad set
including device servers. An equal or lower rank is rejected.

A use of one of those names that is not a direct call is rejected, because it
can hide a send: a function pointer, a parenthesized call (`(open)(p, 0)`), a
macro operand, or any mention on a preprocessor line (a `#define` that
renames or wraps it). Call the function directly and annotate the call. The
scan applies C line splicing first (`op\<newline>en(` is `open(`) and reports
the original line.

## What the gate checks

1. `scripts/check-ipc-send-ranks.py`: recognized send sites are annotated (per
   call site, not per directory); each annotation names a node covering the file;
   the union of annotated edges and the table's `sends_to` edges targets a
   strictly higher rank everywhere; the longest chain is at most 16.
2. `tests/spin/ipc-deadlock.pml`, over the graph generated from that edge set:
   every node is a single-threaded process (the worst case), clients send once
   along each edge, servers forward along all their edges and reply; Spin
   searches for an invalid end state (all live processes blocked). It must find
   none in two passes: with sinks omitted (they only receive and reply, so they
   cannot lie on a wait-for cycle), for every non-empty subset of the clients,
   so any number of clients contend at once; and with every sink modeled as a
   receive-and-reply process, for every single client and pair of clients (three
   or more clients with sinks exceeds the checked state space). Over the acyclic
   shipped graph the model adds little beyond the rank check; its main value is
   showing the cyclic fixture does deadlock.
3. Mutations that must fail: the checked-in fixtures
   `tests/ipc-deadlock/cyclic.tsv` (rank check, and its Spin model deadlocks),
   `tests/ipc-deadlock/lower-rank-site/` (an annotated call site to a lower rank
   with an unedited table) and `tests/ipc-deadlock/unannotated-site/`; the real
   table with `pci` moved below its caller; and scratch trees against the real
   table with an unannotated site, a lower-rank site, a site labeled
   `roottask-core`, and an annotation naming a node that does not own the file, and
   over 60 lexer, wrapper, reference, macro, line-splice and attach-derivation
   cases in `scripts/check-ipc-deadlock.sh` (each must be rejected for its
   stated reason at its stated line, or accepted).

## Gap

The target of a `msg_send(fd, ...)` is asserted by the author: the fd or coid
is a variable, so a site annotated with a higher-rank target than the real one
is not detected, and a reviewer must check it when adding a site. Only a literal
`open` path is derived (see above), and a derived target set can be
under-stated by a redirect the attach table cannot see, such as the
`/dev/console` symlink, which is why extras are allowed. The scan is a lexical
aid, not a coverage proof. It does not see: a libc wrapper outside the list
above (`stat`, `unlink`, `mkdir`, `execve`, `read`/`write` on a received fd), a
send made by a function called through a user-defined pointer (the pointed-to
function holds its own annotated site), inline assembly, or trigraphs and raw
strings. A file the lexer cannot trust (unterminated comment, string or char
literal, C23 digit separator) is rejected rather than skipped. A directory that blocks only via
`read`/`write`/`mmap` on an fd it did not open itself is invisible to the scan
(log output to `/dev/slog` and the console are such sink edges, assumed not to
send); such edges stay in the table's `sends_to`. The Spin model is an
abstraction (single-threaded nodes, rendezvous channels, no timeouts, no
priorities); it does not model the kernel queues, nor worker threads inside a
server (a worker holding a request while it blocks upward must be listed at its
node's rank like any other sender), and with sinks modeled it covers client
pairs only (three clients with sinks exceeded 6 GB; extend it only with a state
reduction). The `roottask-core`/`roottask-fsio` split cannot be enforced per call site
or per message class: both are one endpoint and the dispatcher picks the
handler at run time, and the kernel does not enforce it (see Roles). Revisit it
if the endpoints are ever split.
Dynamic edges (a new server or connection added at run time) must be annotated
or added to the table before they ship.
