#!/usr/bin/env python3
"""Assemble a Perfetto-loadable Chrome trace from an RTMux guest serial log.

`lkl-measure ... json` prints each measured span as a `chrome-event: {...}` line
(see tests/userland/rt-latency/chrome-trace.h).  This lifts those lines out of a
captured guest log, validates each as a Chrome Trace Event, and writes a
`{"traceEvents": [...]}` document that opens directly in the Perfetto UI.

Usage: rt-trace-json.py <guest-log> [-o out.json]   (out defaults to stdout)
Exit status is nonzero if any event is malformed or none are found, so it doubles
as the committed schema validator for the exporter.

SPDX-License-Identifier: Apache-2.0
"""
import argparse
import json
import math
import re
import sys

EVENT_RE = re.compile(r"chrome-event:\s*(\{.*\})\s*$")
STR_FIELDS = ("ph", "name")
INT_FIELDS = ("pid", "tid")
NUM_FIELDS = ("ts", "dur")


def validate(ev, where, errs):
    for k in STR_FIELDS:
        if not isinstance(ev.get(k), str):
            errs.append(f"{where}: field {k!r} missing or not a string")
    for k in INT_FIELDS:
        v = ev.get(k)
        # JSON bools are ints in Python; reject them explicitly.  pid/tid are
        # positive process/track ids.
        if not isinstance(v, int) or isinstance(v, bool) or v <= 0:
            errs.append(f"{where}: field {k!r} missing or not a positive integer")
    for k in NUM_FIELDS:
        v = ev.get(k)
        # A timestamp/duration must be a nonnegative finite number; json.loads
        # otherwise admits NaN/Infinity, which is not valid Chrome-trace JSON.
        if (not isinstance(v, (int, float)) or isinstance(v, bool)
                or not math.isfinite(v) or v < 0):
            errs.append(
                f"{where}: field {k!r} missing or not a nonnegative finite number")
    if ev.get("ph") != "X":
        errs.append(f"{where}: ph is {ev.get('ph')!r}, expected 'X'")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("log", help="captured guest serial log")
    ap.add_argument("-o", "--out", help="output .json (default: stdout)")
    args = ap.parse_args()

    events = []
    errs = []
    with open(args.log, encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            m = EVENT_RE.search(line)
            if not m:
                continue
            where = f"{args.log}:{lineno}"
            try:
                ev = json.loads(m.group(1))
            except json.JSONDecodeError as e:
                errs.append(f"{where}: invalid JSON: {e}")
                continue
            validate(ev, where, errs)
            events.append(ev)

    if errs:
        for e in errs:
            print(e, file=sys.stderr)
        return 1
    if not events:
        print(f"{args.log}: no chrome-event lines found", file=sys.stderr)
        return 1

    doc = {"traceEvents": events, "displayTimeUnit": "ns"}
    text = json.dumps(doc, indent=1)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")
        print(f"rt-trace-json: {len(events)} events -> {args.out}", file=sys.stderr)
    else:
        print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
