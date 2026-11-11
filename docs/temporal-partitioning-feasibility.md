# Temporal Partitioning Feasibility

Decision: park partition-budget capabilities until a named safety-runtime
consumer needs ARINC-653-style windows.

RTMux currently uses fixed-priority scheduling, priority inheritance on
synchronous IPC, tickless idle, and measured QEMU/reference-host budgets. The standing
non-goal still applies: no EDF or `SCHED_DEADLINE` work before the
fixed-priority baseline has hardware evidence. A time-slice capability would be a
scheduling-authority mechanism, not a small extension of endpoint capability
checks.

A minimal future budget cap would gate:

- creating or joining a temporal partition;
- consuming runtime from that partition's replenished budget;
- delegating a smaller child budget to a service;
- revoking a partition and forcing its runnable threads to a fail-closed state.

Priority inheritance would need an explicit rule: donation may raise dispatch
priority inside the current partition, but it must not spend another partition's
budget unless that partition delegated budget authority. Tickless idle would need
the next replenishment deadline in the idle-programming decision, adding a
scheduler timer input that `make check-rt` would have to measure.

Cost:

- TCB delta: scheduler accounting, partition state, revoke handling, and trace
  rows.
- Footprint: one fixed partition table plus per-thread partition id.
- Hot path: budget check on dispatch and timer/replenishment updates.
- Authority source: a held partition-budget cap, not ambient process priority.
- Rollback: keep fixed-priority scheduling and reject partition APIs with
  `ENOSYS`.

Current evidence to preserve: `make check-rt`, `make check-rt-budgets`, the
`deterministic-baseline-runtime` row in `docs/formal-claims.md`, and the
fixed-priority IPC/PI rows in `docs/rt-budgets.md`.

Revisit trigger: a documented safety-runtime workload with at least two temporal
partitions and a required isolation budget that fixed-priority plus PI cannot
state or test.
