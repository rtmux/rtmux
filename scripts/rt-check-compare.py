#!/usr/bin/env python3
"""Compare baseline and instrumentation rt-check logs."""

import argparse
import glob
import re
import sys
from collections import defaultdict
from statistics import median

LINE_RE = re.compile(
    r"^(rt-latency|timer-jitter): .*?"
    r"(?:period_ns=(\d+)|interval_ns=(\d+)).*?"
    r"max_ns=(\d+) p99_ns=(\d+)"
)


def read_logs(pattern):
    rows = defaultdict(list)
    paths = sorted(glob.glob(pattern))
    if not paths:
        raise SystemExit(f"{pattern}: no logs matched")
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                match = LINE_RE.search(line)
                if not match:
                    continue
                probe, period, interval, max_ns, p99_ns = match.groups()
                rows[(probe, int(period or interval))].append((int(max_ns), int(p99_ns)))
    if not rows:
        raise SystemExit(f"{pattern}: no rt-check probe rows found")
    return rows


def ratio_value(num, den):
    if den == 0:
        return float("inf")
    return num / den


def ratio(num, den):
    value = ratio_value(num, den)
    if value == float("inf"):
        return "inf"
    return f"{value:.3f}"


def fmt_num(value):
    if isinstance(value, float) and not value.is_integer():
        return f"{value:.1f}"
    return str(int(value))


def aggregate(values, mode):
    if mode == "median":
        return median(values)
    return max(values)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--base", required=True, help="baseline rt-check log glob")
    ap.add_argument("--instr", required=True, help="instrumented rt-check log glob")
    ap.add_argument("--aggregate", choices=("max", "median"), default="max",
                    help="compare worst run (default) or median across runs")
    ap.add_argument("--max-max-ratio", type=float,
                    help="fail if any instrumentation max/base max ratio is above this")
    ap.add_argument("--max-p99-ratio", type=float,
                    help="fail if any instrumentation p99/base p99 ratio is above this")
    ap.add_argument("--max-max-delta-ns", type=int,
                    help="fail if instrumentation max exceeds baseline max by more than this")
    ap.add_argument("--max-p99-delta-ns", type=int,
                    help="fail if instrumentation p99 exceeds baseline p99 by more than this")
    args = ap.parse_args()

    base = read_logs(args.base)
    instr = read_logs(args.instr)
    keys = sorted(set(base) & set(instr))
    if not keys:
        raise SystemExit("no common rt-check probe rows found")

    failed = False
    prefix = "" if args.aggregate == "max" else "median_"
    print(
        "probe,period_ns,base_runs,instr_runs,"
        f"base_{prefix}max_ns,instr_{prefix}max_ns,max_ratio,max_delta_ns,"
        f"base_{prefix}p99_ns,instr_{prefix}p99_ns,p99_delta_ns,p99_ratio"
    )
    for key in keys:
        b = base[key]
        i = instr[key]
        base_max = aggregate([v[0] for v in b], args.aggregate)
        instr_max = aggregate([v[0] for v in i], args.aggregate)
        base_p99 = aggregate([v[1] for v in b], args.aggregate)
        instr_p99 = aggregate([v[1] for v in i], args.aggregate)
        max_ratio = ratio_value(instr_max, base_max)
        p99_ratio = ratio_value(instr_p99, base_p99)
        max_delta = instr_max - base_max
        p99_delta = instr_p99 - base_p99
        print(
            f"{key[0]},{key[1]},{len(b)},{len(i)},"
            f"{fmt_num(base_max)},{fmt_num(instr_max)},"
            f"{ratio(instr_max, base_max)},"
            f"{fmt_num(max_delta)},"
            f"{fmt_num(base_p99)},{fmt_num(instr_p99)},"
            f"{fmt_num(p99_delta)},"
            f"{ratio(instr_p99, base_p99)}"
        )
        if args.max_max_ratio is not None and max_ratio > args.max_max_ratio:
            print(
                f"rt-check-compare: {key[0]} {key[1]} max_ratio "
                f"{max_ratio:.3f} exceeds {args.max_max_ratio:.3f}",
                file=sys.stderr,
            )
            failed = True
        if (args.max_max_delta_ns is not None and
                max_delta > args.max_max_delta_ns):
            print(
                f"rt-check-compare: {key[0]} {key[1]} max_delta_ns "
                f"{fmt_num(max_delta)} exceeds {args.max_max_delta_ns}",
                file=sys.stderr,
            )
            failed = True
        if args.max_p99_ratio is not None and p99_ratio > args.max_p99_ratio:
            print(
                f"rt-check-compare: {key[0]} {key[1]} p99_ratio "
                f"{p99_ratio:.3f} exceeds {args.max_p99_ratio:.3f}",
                file=sys.stderr,
            )
            failed = True
        if (args.max_p99_delta_ns is not None and
                p99_delta > args.max_p99_delta_ns):
            print(
                f"rt-check-compare: {key[0]} {key[1]} p99_delta_ns "
                f"{fmt_num(p99_delta)} exceeds {args.max_p99_delta_ns}",
                file=sys.stderr,
            )
            failed = True
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
