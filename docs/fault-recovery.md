# RTMux Fault Event And Recovery Model

This note defines the current structured fault-event contract. It is a runtime
contract, not a promise that every recovery policy is complete. Rows marked
`specified` define the required behavior and name the current implementation or
evidence boundary; rows marked `tested` have an executable gate.

## Event Record

Every recoverable fault decision has these fields:

| field | meaning |
| --- | --- |
| event_id | Stable event class from the taxonomy below. |
| severity | `deny`, `thread-fatal`, `service-fatal`, or `system-fatal`. |
| subject | Process, thread, endpoint, service path, or kernel subsystem deciding the event. |
| cause | Trap cause, errno, denied capability, watchdog name, or crashed service generation. |
| action | Kernel or supervisor action before the subject can run again. |
| authority_delta | Must be `none`; recovery must not grant new authority. |
| evidence | Test, proof, or design row that currently backs the contract. |

## Recovery Rules

- The kernel fault path may mark a thread/process dead, return an errno, or
  force-ready waiters, but it must not allocate dynamically to decide the fault.
- A user thread that takes an unrecoverable CPU exception is reported through
  wait status before the parent can reap it.
- Capability denial returns an errno and leaves object authority unchanged.
- Service restart is user-mode policy. A replacement service must reacquire its
  resources and publish a fresh generation; stale handles must fail closed.
- Watchdogs may stop or fail a probe, but a watchdog expiry is not yet a general
  kernel recovery event.
- Owner-death handling exists in the sync teardown path, but full robust-mutex
  `EOWNERDEAD` semantics remain specified rather than claimed complete.

## Taxonomy

| event id | severity | trigger | kernel action | supervisor notification | recovery option | immutable log fields | allocation rule | authority delta | evidence | status |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| illegal-syscall | deny | Unsupported private operation from an unprivileged process. | Return capability-masked `EPERM`; no object mutation. | None. | Caller handles errno. | slot, pid, tid, errno. | No dynamic allocation. | none | `scripts/qemu-regress.sh` mode `fault-recovery` checks an unsupported private call from an unprivileged process. | tested |
| invalid-capability | deny | Malformed, stale, or unauthorized capability handle. | Reject the operation with `EBADF` or `EPERM`; do not resolve an object pointer. | None. | Caller handles errno; held caps unchanged. | cap type if decoded, pid, tid, errno. | No dynamic allocation. | none | `scripts/qemu-regress.sh` mode `fault-recovery`; `tests/userland/capgate.c`; `make check-cap-namespace`. | tested |
| user-page-fault | thread-fatal | User instruction/load/store page fault outside a recoverable copy window. | Stop the faulting thread/process and report `SIGSEGV` through wait status. | Parent observes wait status. | Parent reaps or restarts the process. | pid, tid, signal, fault address, pc. | No dynamic allocation in the trap decision. | none | `scripts/qemu-regress.sh` mode `fault-recovery`; `tests/userland/main.c` fault fixtures. | tested |
| watchdog-expiry | service-fatal | A bounded probe's watchdog observes no forward progress before its deadline. | The probe fails closed; general kernel restart policy is not implemented. | Probe log names the stalled phase. | Supervisor or test runner restarts the workload. | watchdog name, phase, deadline, pid/tid if known. | Watchdog decision uses preallocated probe state. | none | `tests/userland/rt-latency/preempt-latency.c`; `docs/rt-budgets.md`. | specified |
| owner-death | deny | A thread/process exits while owning a sync object or service generation. | Force-ready waiters or mark the sync object dead; stale service handles fail closed. | Waiter errno or service health-path disappearance. | Waiter handles `EOWNERDEAD`/errno; supervisor may restart service. | owner pid/tid, object address or service path, errno. | Sync teardown uses existing wait queues; no allocation in the wake decision. | none | `kernel/sync/core.c`; `tests/userland/rt-latency/logger-restart.c`. | specified |
| server-crash | service-fatal | User-mode service exits or faults after publishing a path. | Kernel/roottask reclaim process-owned endpoints, connections, caps, and resource claims. | Health path disappears or old handle returns `EBADF`/documented errno. | User-mode supervisor starts a new generation that reacquires resources. | service path, old pid, new pid, stale-handle errno. | Restart policy runs in user mode; kernel reclamation uses existing teardown state. | none | `scripts/qemu-regress.sh` mode `logger-restart`; `tests/userland/rt-latency/logger-restart.c`; `docs/service-frontend-abi.md`. | tested |

## Claim Limits

The fault taxonomy is the required event vocabulary for new recovery work. It
does not claim whole-system availability, automatic respawn, complete POSIX
robust-mutex semantics, or a kernel-managed service supervisor. Any new row must
name the authority delta, allocation rule, and executable evidence before its
status can be `tested`.
