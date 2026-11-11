"""Shared decoding helpers for the tracelogger post-processors.

scripts/rt-trace-{sched,irq,ctf,lkl-host}.py each parse the same raw event
stream from /bin/tracelogger, so the header decode and the cycle-to-nanosecond
conversion were copied byte-for-byte into all of them. They are here once
instead.

Deliberately not shared: rt-trace-ctf.py keeps its own parse_u32, because it
rejects out-of-range values rather than masking them (a malformed CTF record is
worth failing on, while the CSV summarizers tolerate one). percentile lives here
even though the CTF exporter does not use it, since every parser that reports a
p99 must agree on what one means.

Imported by filename from the same directory, which works because the scripts
are run as `python3 scripts/rt-trace-*.py` and so put scripts/ on sys.path.
"""

import math
import re

CYCLES_PER_SEC_RE = re.compile(r"init_qtime:\s+.*\bcycles_per_sec=(\d+)\b")


def parse_u32(text):
    """Parse a decimal or 0x literal and truncate to 32 bits."""
    return int(text, 0) & 0xFFFFFFFF


def read_cycles_per_sec(path):
    """Recover the guest timebase from the boot log, or 0 if absent.

    Zero means "leave durations in raw ticks": cycles_to_ns below passes them
    through, which keeps the parsers usable on a log with no init_qtime line.
    """
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            match = CYCLES_PER_SEC_RE.search(line)
            if match:
                cycles_per_sec = int(match.group(1))
                if cycles_per_sec > 0:
                    return cycles_per_sec
    return 0


def cycles_to_ns(cycles, cycles_per_sec):
    """Scale a tick count to nanoseconds; identity when the rate is unknown.

    The counter must be the one cycles_per_sec describes. On RISC-V that is the
    time CSR, which the kernel reads through cpu_clock_ticks(); clock_cycles()
    reads the separate cycle CSR and its deltas must not be scaled here.
    """
    if not cycles_per_sec:
        return cycles
    return cycles * 1000000000 // cycles_per_sec


def trace_class(header):
    """Event class from a raw trace header word."""
    return (header >> 10) & 0x1F


def trace_event(header):
    """Event number within its class from a raw trace header word."""
    return header & 0x3FF


def percentile(values, pct):
    """Nearest-rank percentile: the smallest sample at or above pct percent."""
    if not values:
        return 0
    values = sorted(values)
    idx = max(0, math.ceil(len(values) * pct / 100) - 1)
    return values[idx]
