# RTMux Safety Runtime Charter

This charter is the evidence spine for safety-runtime claims. It records the
current claim boundary, the evidence already in-tree, and the gaps that must stay
visible until an external assessment exists.

## TCB Boundary

The trusted computing base is the S-mode kernel under `kernel/` and
`arch/riscv/`, the boot path that creates the first user task, the bootstrap
capability manifest, and the roottask policy needed to load and supervise service
processes. User-mode filesystems, LKL domains, drivers, shells, and POSIX
compatibility code are outside the kernel TCB; they receive only the capabilities
their manifests or parents grant.

## Assumptions Of Use

- RV64 LP64 is the only supported ABI shape.
- The platform firmware and QEMU/FU740 hardware description honestly describe
  memory, interrupt, timer, and MMIO resources.
- Initial authority is derived from the boot image and bootstrap capability
  manifest before ordinary services run.
- Measurements in `docs/rt-budgets.md` are engineering evidence for the named
  target/configuration, not portable hardware latency guarantees.
- Fixed bounds relied on by claims are categorized in `docs/assumptions.md`.

## Hazards

- Ambient authority accidentally bypasses held capabilities.
- A recycled object, pid, thread id, endpoint, or capability handle authorizes a
  stale caller.
- SMP publication or reclamation lets a hart observe freed or partially
  initialized state.
- User-controlled bytes, lengths, or device data cross a parser or copy boundary
  without a checked bound.
- Recovery from a fault or service crash widens authority or leaves a runnable
  faulted thread without a recorded decision.

## Non-Goals

- External safety approval and whole-kernel machine proof are out of scope;
  see Limitations for current status.
- Linux driver behavior inside LKL domains is contained reuse, not part of the
  native realtime scheduling, IPC, or interrupt path.
- POSIX breadth is limited to generated, claimed profile rows.
- QEMU timing rows do not replace board-specific hardware evidence.

## Claim Limits

Claims may be `Proven`, `Tested`, `Asserted`, or `Aspirational` only in
`docs/formal-claims.md`. A row is valid only when it names the code boundary,
backing evidence, bound, model gap, and revisit trigger. This charter may point
to those rows, but it must not upgrade their status.

## Residual Risks

- Controlled fault recovery is specified by `docs/fault-recovery.md`, with only
  the rows marked `tested` carrying executable recovery evidence today.
- Deterministic-execution evidence is indexed from existing realtime and
  footprint rows; it is a QEMU/TCG engineering baseline unless a row names
  hardware evidence.
- FU740 absolute timing rows remain board-gated where QEMU-TCG cannot provide a
  physical latency bound.
- LKL domain behavior remains larger and less deterministic than native RTMux
  services, so LKL use must keep containment and latency gates attached.

## Trace Matrix

| claim id | claim class | boundary | assumption boundary | design note | evidence artifact | residual risk |
| --- | --- | --- | --- | --- | --- | --- |
| capability-law-unforgeability | strong-isolation | `kernel/cap/table.c`, endpoint/process/thread cap users | Held capability handles and current generation encoding. | `docs/capability-law.md` | `make host-tests`; `tests/userland/capgate.c`; `make check-cap-namespace` | Runtime tests, not whole-kernel proof. |
| capability-law-type-discrimination | strong-isolation | Typed cap lookup and endpoint/process/thread users. | Current object type tags and cap-table layout. | `docs/capability-law.md` | `make host-tests`; `make check-cap-namespace` | Future cap types need new rows. |
| capability-law-rights-enforcement | strong-isolation | Cap lookup and authority gates. | Callers request explicit nonzero rights. | `docs/capability-law.md` | `make host-tests`; `tests/userland/capgate.c`; `make check-cap-namespace` | Does not prove future callers choose minimal rights. |
| capability-law-attenuation-only | strong-isolation | Delegation and transfer helpers. | Same-table delegation and cross-table transfer algebra. | `docs/capability-law.md` | `make check-cap-smt`; `make host-tests` | SMT abstracts generation and pin lifetime. |
| capability-law-transfer-without-grant | strong-isolation | Native cap transfer vector validation. | `_IO_NATIVE_CAP_TRANSFER_MAX` row in `docs/assumptions.md`. | `docs/capability-law.md` | `make check-cap-smt`; `make host-tests` | Allocation rollback is host-tested, not in SMT. |
| capability-law-revoke-before-free | strong-isolation | Revocation groups and object final unpin. | Current group-id revoke and pin-count delayed free model. | `docs/capability-law.md` | `make check-cap-smt`; `make host-tests` | SMT omits all concrete object users. |
| capability-law-stale-handle-rejection | strong-isolation | Generation checks, detach, revoke, object death. | Current generation counter and stale endpoint/process behavior. | `docs/capability-law.md` | `make host-tests`; `tests/userland/capgate.c`; `make check-cap-namespace` | No exhaustive proof over every userspace handle. |
| capability-law-no-ambient-lookup | strong-isolation | Kernel private gates and global channel lookup. | Kernel syscalls use held caps or caller self only. | `docs/capability-abi.md` | `tests/userland/capgate.c`; `make check-capability-abi` | Focused ABI/QEMU coverage, not noninterference proof. |
| loader-image-policy | strong-isolation | Roottask executable loader and bootstrap manifest grants. | Executable authority derives from the opened boot-cpio image path and hash, not caller text. | `docs/loader-policy.md`; `docs/boot-manifest.md` | `make check-loader-negative`; `make check-boot-manifest` | Negative QEMU coverage covers malformed ELF and `execvp()` fallback, not a proof of future dynamic-loading extensions. |
| deterministic-baseline-runtime | deterministic-execution | Scheduler, IPC rendezvous, timer, trace export, and built image footprint. | QEMU/reference-host configurations named by each budget row; footprint is built image size, not RSS. | `docs/rt-budgets.md`; `docs/smp-contract.md` | `make check-rt`; `make check-rt-budgets`; `make footprint-report`; `make check-trace-export` | QEMU/TCG rows are not portable hardware latency guarantees; hardware latency claims still need board evidence. |
| memory-isolation | strong-isolation | User-pointer probes, per-process address spaces, capability-gated global channels. | Callers pass pointers the kernel must not trust; spatial separation rests on Sv39 and the probe helpers. | `docs/capability-abi.md` | `tests/userland/user-ptr-negative.c`; `tests/userland/capgate.c`; `make check-cap-namespace` | Negative tests are per-site, not an exhaustive proof; DMA and MMIO mappings have their own gates. |
| object-lifecycle-teardown | strong-isolation | Process reap, connection refcount, and blocked-thread teardown ordering. | The hand-written transition model matches the teardown paths named in the formal-claims row; it is sequential. | `docs/smp-contract.md` | `tests/smt/lifecycle.smt2`; `make check-lifecycle-smt` | The model is not the C code and proves no race freedom; atomicity and ordering rest on CBMC, litmus and runtime tests. |
| ipc-containment | strong-isolation | Synchronous IPC rendezvous, channel and connection lifetime, capability transfer. | Modelled rendezvous states match `kernel/ipc/`; payloads are abstracted. | `docs/smp-contract.md` | `tests/spin/ipc-rendezvous.pml`; `make check-ipc-cbmc`; `make check-ipc-litmus` | Models abstract the bridge bits and message contents; litmus tests cover memory ordering only. |
| fixed-priority-scheduling | deterministic-execution | Fixed-priority and round-robin dispatch, priority inheritance on synchronous IPC. | The abstract SRR priority-inheritance model matches the dispatcher; rr-fairness runs one named workload. | `docs/rt-budgets.md` | `tests/smt/srr-pi-safety.smt2`; `make check-srr-pi-smt`; `scripts/qemu-regress.sh` (mode rr-fairness) | The model is not the dispatcher and QEMU timing is not hardware timing. |
| ipc-deadlock-freedom | deterministic-execution | Blocking server-to-server sends (`MsgSendv`) across the shipped servers. | Every `msg_send*`, `MsgSend*`, or `open()` call site under `servers/` and `userland/` carries an `ipc-send:` annotation naming its source node and targets; the scan rejects an unannotated site or an edge to an equal or lower rank without a table edit. The annotated target is asserted by the author. | `docs/ipc-deadlock-freedom.md` | `make check-ipc-deadlock` | Annotations are author-asserted, not derived from the code; the Spin model is single-threaded per node and checks client pairs with sinks and any client subset without; the roottask role split is enforced per call site, not per message class, and not by the kernel. |
| syscall-validation | safety-runtime | Syscall argument validation across `kernel/`. | Converted sites validate through `user_slice` (`docs/user-slice-inventory.md`); the not-converted list still validates per site. | `docs/capability-abi.md` | `tests/userland/user-ptr-negative.c`; `make check-user-slice` | The CBMC model covers the range predicate only, and only converted sites use it. |
| fault-containment | controlled-recovery | User fault delivery, server crash handling, capability-denial paths. | Fault classes exercised are the four in the smoke mode; recovery decisions are logged. | `docs/fault-recovery.md` | `scripts/qemu-regress.sh` (mode fault-recovery) | A smoke test of four fault classes; server-crash and watchdog recovery have separate regressions. |
| controlled-recovery-stale-handles | controlled-recovery | Restarted services, stale endpoint handles, crashed LKL domains. | Restart probes use bounded workloads and fixed service paths. | `docs/fault-recovery.md`; `docs/memory-safety-inventory.md`; `docs/lkl-domain.md` | `make check-fault-recovery`; `make check-lkl-containment` | Specified recovery rows without executable evidence remain design contracts. |
| safety-runtime-evidence-spine | safety-runtime | Charter, formal-claims ledger, assumptions, budgets, and inventories. | Claims are valid only when their row has evidence and a residual risk. | `docs/safety-runtime.md` | `scripts/check-evidence-claims.sh`; `make check-evidence-claims` | The checker proves traceability, not correctness of mechanisms. |

## Limitations

RTMux is not certified, certification-ready, or compliant with any external
safety standard today. Certification-oriented evidence may be generated later,
but until an independent program exists, this charter is only an internal
engineering evidence map.
