#!/usr/bin/env python3
"""Summarize IRQ claim to IST dispatch latency from tracelogger output."""

import argparse
import re
import statistics
import sys
from collections import defaultdict, deque

from rt_trace_common import (  # shared with the other rt-trace parsers
    cycles_to_ns,
    parse_u32,
    percentile,
    read_cycles_per_sec,
    trace_class,
    trace_event,
)

EVENT_RE = re.compile(
    r"tracelogger:\s+buf\s+\d+\s+ev\s+\d+\s+"
    r"header=(0x[0-9a-fA-F]+|\d+)\s+"
    r"d=(0x[0-9a-fA-F]+|\d+),(0x[0-9a-fA-F]+|\d+),(0x[0-9a-fA-F]+|\d+)"
)

TRACE_INT_C = 3
IRQ_CLAIM = 5
IST_DISPATCH = 6


def read_irq_samples(path, cycles_per_sec):
    pending = defaultdict(deque)
    samples = []
    unmatched_dispatch = 0
    last_raw = None
    wrap_base = 0
    seen = set()

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            match = EVENT_RE.search(line)
            if not match:
                continue
            header, d0, d1, d2 = (parse_u32(v) for v in match.groups())
            key = (header, d0, d1, d2)
            if key in seen:
                continue
            seen.add(key)
            if last_raw is not None and d0 < last_raw and last_raw - d0 > 0x80000000:
                wrap_base += 0x100000000
            last_raw = d0
            ts = wrap_base + d0

            if trace_class(header) != TRACE_INT_C:
                continue
            ev = trace_event(header)
            if ev == IRQ_CLAIM:
                irq = d1
                pid = d2
                pending[pid].append((ts, irq))
            elif ev == IST_DISPATCH:
                pid = d1
                tid = d2
                queue = pending.get(pid)
                if not queue:
                    unmatched_dispatch += 1
                    continue
                start, irq = queue.popleft()
                samples.append(
                    (cycles_to_ns(ts - start, cycles_per_sec), irq, pid, tid)
                )

    unmatched_claim = sum(len(v) for v in pending.values())
    return samples, unmatched_claim, unmatched_dispatch


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="guest serial log containing tracelogger lines")
    args = ap.parse_args()

    cycles_per_sec = read_cycles_per_sec(args.log)
    samples, unmatched_claim, unmatched_dispatch = read_irq_samples(
        args.log, cycles_per_sec
    )
    print("irq,pid,count,min_ns,p50_ns,p99_ns,max_ns")
    by_key = defaultdict(list)
    for duration, irq, pid, _tid in samples:
        by_key[(irq, pid)].append(duration)
    for irq, pid in sorted(by_key):
        values = by_key[(irq, pid)]
        print(
            f"{irq},{pid},{len(values)},{min(values)},"
            f"{int(statistics.median(values))},{percentile(values, 99)},"
            f"{max(values)}"
        )
    if unmatched_claim or unmatched_dispatch:
        print(
            f"warning: unmatched_claim={unmatched_claim} "
            f"unmatched_dispatch={unmatched_dispatch}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
