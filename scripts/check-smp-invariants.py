#!/usr/bin/env python3
"""Check that the SMP invariant ledger covers the required shared objects."""

from pathlib import Path

ROWS = (
    "process_t",
    "thread_t",
    "cap table",
    "channel/connect",
    "page tables",
    "run queues",
    "RCU lookups",
    "IRQ delivery",
    "timers",
    "revoke paths",
)

EVIDENCE = (
    "make check-lock-rank",
    "make check-rcu-sched",
    "make check-capability-law",
    "scripts/qemu-regress.sh --mode smp-invariants",
    "CPUS=8 scripts/qemu-regress.sh --mode rcu-lookup-measure",
    "CPUS=8 scripts/qemu-regress.sh --mode revoke-stress",
    "CPUS=8 scripts/qemu-regress.sh --mode preempt-latency",
    "CPUS=8 scripts/qemu-regress.sh --mode ipc-contention",
)

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    doc = (ROOT / "docs/smp-invariants.md").read_text()
    errors = []

    for row in ROWS:
        if f"| {row} |" not in doc:
            errors.append(f"docs/smp-invariants.md: missing row {row}")
    for token in EVIDENCE:
        if token not in doc:
            errors.append(f"docs/smp-invariants.md: missing evidence {token}")

    if errors:
        for error in errors:
            print(error)
        return 1
    print("check-smp-invariants: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
