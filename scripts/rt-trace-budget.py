#!/usr/bin/env python3
"""Emit rt-budget blocks from rt-trace-sched.py measure_summary rows."""

import argparse
import csv
import re
import sys
from pathlib import Path


CLASS_NAMES = {
    "fs": "LKL ext4 traced request round trip",
    "fs-rt": "LKL ext4 traced under-load native wake",
    "net": "LKL network traced request round trip",
    "net-rt": "LKL network traced under-load native wake",
    "net-blocked": "LKL network traced blocked-poll timeout",
    "net-revoke": "LKL network traced revoke",
    "pi_boost": "priority-inheritance boost span",
}

BUDGET_NS = {
    "fs": 2000000,
    "fs-rt": 2000000,
    "net": 2000000,
    "net-rt": 2000000,
}

RAW_RE = re.compile(
    r"lkl-measure:\s+mode=([A-Za-z0-9_-]+)\s+samples=(\d+)\s+"
    r"max_ns=(\d+)\s+p99_ns=(\d+)"
)


def measure_rows(path):
    with path.open(newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.reader(f):
            if not row or row[0] != "measure_summary":
                continue
            if row[1] == "mode":
                continue
            if len(row) != 7:
                raise SystemExit(f"{path}: malformed measure_summary row: {row}")
            yield {
                "mode": row[1],
                "count": row[2],
                "p99": row[5],
                "max": row[6],
            }


def lkl_host_rows(path):
    """Yield budget rows from an rt-trace-lkl-host.py CSV.

    That CSV has op,count,min_ns,p50_ns,p99_ns,max_ns per host-op type. Map each
    op to an lkl_host_<op> mode so emit_block gives it a host wait-span class.
    """
    with path.open(newline="", encoding="utf-8", errors="replace") as f:
        for row in csv.reader(f):
            if not row or row[0] == "op" or row[0].startswith("warning"):
                continue
            if len(row) != 6:
                raise SystemExit(f"{path}: malformed lkl-host row: {row}")
            yield {
                "mode": f"lkl_host_{row[0]}",
                "count": row[1],
                "p99": row[4],
                "max": row[5],
            }


def raw_measure_rows(path):
    if path is None:
        return {}
    rows = {}
    text = path.read_text(encoding="utf-8", errors="replace")
    for match in RAW_RE.finditer(text):
        mode = match.group(1)
        rows[mode] = {
            "count": match.group(2),
            "max": match.group(3),
            "p99": match.group(4),
        }
    return rows


def merge_raw(row, raw_rows):
    raw = raw_rows.get(row["mode"])
    if not raw:
        return row
    return {
        "mode": row["mode"],
        "count": raw["count"],
        "p99": raw["p99"],
        "max": raw["max"],
    }


# Platform citation per row class, never per tracelogger mode: a run can mix
# classes (an LKL run also yields kcall rows), and each row must cite what that
# row rests on.  kcall and pi_boost spans are rdtime deltas and nothing else,
# so their field is fixed here.  fs, net and host each take their own explicit
# option with no fallback to another class, and a mode that maps to no class is
# an error rather than a default.  Interrupt latency is not a measure_summary
# row (it is reported in measure_irq_summary, which this script does not read),
# so there is deliberately no irq class.
TIMER_ONLY = "qemu-only PB-TIMER-RDTIME"
OPTION_FOR = {"fs": "platform_fs", "net": "platform_net",
              "host": "platform_host"}


def row_class(mode):
    """Platform class of one measure row, or None when it is not classified."""
    if mode == "pi_boost" or mode.startswith("kcall_"):
        return "timer"
    if mode.startswith("lkl_host_"):
        return "host"
    if mode in ("fs", "fs-rt"):
        return "fs"
    if mode in ("net", "net-rt", "net-blocked", "net-revoke"):
        return "net"
    return None


def platform_for(args, mode):
    """Platform field for one block, or SystemExit when it cannot be named."""
    cls = row_class(mode)
    if cls is None:
        raise SystemExit(f"rt-trace-budget: row class of mode '{mode}' has no "
                         "platform contract; classify it in row_class()")
    if cls == "timer":
        return TIMER_ONLY
    val = getattr(args, OPTION_FOR[cls])
    if not val:
        raise SystemExit(f"rt-trace-budget: mode '{mode}' is a {cls} row and "
                         f"needs --platform-{cls}")
    return val


def emit_block(args, path, row):
    mode = row["mode"]
    if mode in CLASS_NAMES:
        cls = CLASS_NAMES[mode]
    elif mode.startswith("kcall_"):
        # Per-ecall-execution kernel-side cost for one kernel call number; see
        # print_kcalls() in rt-trace-sched.py for the restart-accounting rule.
        cls = f"kcall-{mode[len('kcall_'):]}"
    elif mode.startswith("lkl_host_"):
        cls = f"LKL host {mode[len('lkl_host_'):]} wait span"
    else:
        cls = f"LKL {mode} traced measurement"
    print("```rt-budget")
    print(f"class: {cls}")
    print(f"command: {args.command}")
    print(f"config-hash: {args.config_hash}")
    print(f"target: {args.target}")
    print(f"platform: {platform_for(args, mode)}")
    print(f"opensbi: {args.opensbi}")
    print(f"kernel-git-sha: {args.kernel_git_sha}")
    if args.date:
        print(f"date: {args.date}")
    print(f"samples: {row['count']}")
    print(f"max-ns: {row['max']}")
    print(f"p99-ns: {row['p99']}")
    if mode in BUDGET_NS:
        print(f"budget-ns: {BUDGET_NS[mode]}")
    print(f"histogram-artifact: {path}")
    print("```")
    print()


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    ap.add_argument("sched_csv", nargs="+", type=Path)
    ap.add_argument("--command", required=True)
    ap.add_argument("--config-hash", required=True)
    ap.add_argument("--target", required=True)
    for cls in OPTION_FOR:
        ap.add_argument(f"--platform-{cls}", default=None,
                        help=f"platform field for {cls} rows: <class> PB-ID"
                        "[, PB-ID...] from docs/platform-boundary-contracts.md; "
                        "no default and no fallback to another row class")
    ap.add_argument("--opensbi", required=True)
    ap.add_argument("--kernel-git-sha", required=True)
    ap.add_argument("--date", help="optional YYYY-MM-DD; omitted from the block when unset")
    ap.add_argument("--guest-log", type=Path,
                    help="guest serial log with raw lkl-measure rows")
    ap.add_argument("--lkl-host", type=Path,
                    help="rt-trace-lkl-host.py CSV for host-op wait-span blocks")
    ap.add_argument("--allow-empty", action="store_true",
                    help="a run with nothing to measure is not an error")
    args = ap.parse_args(argv)
    with open(Path(__file__).with_name("platform-field.ere")) as f:
        field_re = re.compile(f.read().strip())
    for cls, dest in OPTION_FOR.items():
        opt, val = f"--platform-{cls}", getattr(args, dest)
        if val is None:
            continue
        if not field_re.fullmatch(val):
            ap.error(f"{opt} must be `<class> PB-ID[, PB-ID...]`")
        if val.startswith("production "):
            ap.error(f"{opt}: generated blocks come from a harness run and "
                     "cannot claim `production`; edit the block by hand with "
                     "hardware evidence")

    raw_rows = raw_measure_rows(args.guest_log)
    rows = []
    for path in args.sched_csv:
        for row in measure_rows(path):
            rows.append((path, merge_raw(row, raw_rows)))
    if args.lkl_host is not None:
        for row in lkl_host_rows(args.lkl_host):
            rows.append((args.lkl_host, row))
    # name every platform field before printing anything, so an unclassified
    # row leaves no half-written output behind
    for _path, row in rows:
        platform_for(args, row["mode"])
    for path, row in rows:
        emit_block(args, path, row)
    emitted = len(rows)
    if emitted == 0:
        print("rt-trace-budget: no measure_summary rows found", file=sys.stderr)
        return 0 if args.allow_empty else 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
