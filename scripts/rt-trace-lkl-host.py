#!/usr/bin/env python3
"""Summarize LKL host-operation waits from tracelogger serial output."""

import argparse
import re
import statistics
import sys

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

TRACE_USER_C = 6
USER_LKL_HOST_ENTER = 0x200
USER_LKL_HOST_EXIT = 0x201

OP_NAMES = {
    1: "page_alloc",
    2: "page_free",
    3: "sem_down",
    4: "mutex_lock",
    5: "thread_join",
    6: "timer_set_oneshot",
    7: "mmap",
    8: "munmap",
}


def summarize(path):
    pending = {}
    samples = {}
    unmatched_exit = 0
    cycles_per_sec = read_cycles_per_sec(path)

    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            match = EVENT_RE.search(line)
            if not match:
                continue
            header, d0, d1, d2 = (parse_u32(v) for v in match.groups())
            if trace_class(header) != TRACE_USER_C:
                continue
            ev = trace_event(header)
            if ev == USER_LKL_HOST_ENTER:
                pending.setdefault(d1, []).append((d0, d2))
            elif ev == USER_LKL_HOST_EXIT:
                entries = pending.get(d1)
                if not entries:
                    unmatched_exit += 1
                    continue
                # sem_down and mutex_lock carry the same lock pointer on both
                # enter and exit, so pair by object identity to keep overlapping
                # same-op waits from cross-pairing. Ops whose exit value differs
                # from the enter value (thread_join, timer, allocs) fall back to
                # the oldest open enter of that op.
                idx = next(
                    (i for i, (_, value) in enumerate(entries) if value == d2),
                    0,
                )
                start, value = entries.pop(idx)
                # The trace timestamp is a raw counter; mask a single 32-bit wrap
                # of the delta, then scale ticks to nanoseconds so the _ns columns
                # are real nanoseconds rather than counter ticks.
                ticks = (d0 - start) & 0xFFFFFFFF
                span = cycles_to_ns(ticks, cycles_per_sec)
                samples.setdefault(d1, []).append((span, value, d2))

    unmatched_enter = sum(len(v) for v in pending.values())
    return samples, unmatched_enter, unmatched_exit


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("log", help="guest serial log containing tracelogger lines")
    args = ap.parse_args()

    samples, unmatched_enter, unmatched_exit = summarize(args.log)
    print("op,count,min_ns,p50_ns,p99_ns,max_ns")
    if not samples:
        return 0

    for op in sorted(samples):
        durations = [s[0] for s in samples[op]]
        print(
            f"{OP_NAMES.get(op, f'op_{op}')},{len(durations)},"
            f"{min(durations)},{int(statistics.median(durations))},"
            f"{percentile(durations, 99)},{max(durations)}"
        )
    if unmatched_enter or unmatched_exit:
        print(
            f"warning: unmatched_enter={unmatched_enter} "
            f"unmatched_exit={unmatched_exit}",
            file=sys.stderr,
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
