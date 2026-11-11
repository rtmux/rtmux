# Zero-Copy Streaming Ring Feasibility

Decision: park implementation until LKL net or block throughput data shows the
copy path is the ceiling.

Baseline to beat: the existing `native IPC MsgSend round trip, page payload 4096
B` row in `docs/rt-budgets.md`. That row measures the current copy+rendezvous
path. A shared ownership-transfer ring only earns code if one streaming workload
beats that baseline while preserving capability isolation.

Paper estimate: for 4 KiB packets or blocks, a shared ring removes the data copy
and per-buffer `MsgSend`, replacing them with descriptor validation, release /
acquire index updates, and an occasional notification doorbell. That can win for
batched LKL net RX/TX or block bulk I/O. It is unlikely to win for small control
messages, where SRR already gives bounded priority donation and simpler failure
semantics.

Security invariant: every ring index, wrap generation, flag, and descriptor is
untrusted input. The consumer bounds-checks each descriptor offset+length against
the transferred memory-object extent with a single read before use. Mid-stream
revoke fails closed. Ownership moves by transferring the memory-object
capability, so a completed handoff leaves no shared writable window.

Efficiency gate:

- TCB delta: no new kernel object; user-mode ring code plus existing
  memory-object cap transfer and notification doorbell.
- Hot-path cost: no per-message syscall, only user-mode index manipulation and
  batched doorbell wakeups.
- Authority source: transferred memory-object capability with map rights.
- Rollback: delete the ring path and use copy+rendezvous if it fails to beat the
  4096 B `MsgSend` baseline or violates the stale/revoke tests.

Implementation remains blocked. Required evidence before building: a measured
LKL net or block workload showing copy+rendezvous is the throughput ceiling, plus
a proposed `lkl-stream-throughput` mode that reports ring enqueue/dequeue,
doorbell wake, revoke drain p99/max, and unrelated high-priority IPC impact.
