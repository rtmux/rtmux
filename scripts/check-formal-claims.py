#!/usr/bin/env python3
"""Validate the row schema of docs/formal-claims.md.

Every row needs all seven base fields non-empty and a status in
{Proven, Tested, Asserted, Aspirational}; an Aspirational row needs a revisit
trigger (already one of the seven). Every row also needs a `layer` column naming
one of LAYERS, and a claim table for the real document must cover every layer
with at least one row. A `platform` column (`n/a` or `<class> PB-ID[, ...]`) is
also required when layer coverage is enforced, and validated wherever present;
ids are resolved by scripts/check-platform-boundary-contracts.sh. A row whose id
carries one of the tags
sched-abi, ipc-structure, cap-structure, or info-flow must also have a
`tag-fields` column naming a proof target, regression command, budget row,
POSIX impact, and rollback threshold.

Usage: check-formal-claims.py [--expect-fail] [--layers] [FILE ...]
With --expect-fail the run succeeds only if every FILE is rejected (negative
fixtures). Layer coverage is enforced for the default FILE
(docs/formal-claims.md) and for any FILE when --layers is given; fixtures with
one row skip it otherwise. A pass over the default file records the validated
row ids and layers for the gate certificate (see gate_cert.emit).
"""

import argparse
import os
import re
import sys
from collections import Counter
from pathlib import Path

import gate_cert

BASE = ["id", "status", "code boundary", "backing evidence", "bound", "gap",
        "revisit trigger"]
STATUS = {"Proven", "Tested", "Asserted", "Aspirational"}
LAYERS = ("capability", "loader", "runtime-determinism", "memory-isolation",
          "ipc", "scheduling", "syscall-boundary", "fault-containment")
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "platform-field.ere")) as _f:
    PLATFORM_RE = re.compile("n/a|" + _f.read().strip())
TAGS = ("sched-abi", "ipc-structure", "cap-structure", "info-flow")
TAG_FIELDS = ("proof target", "regression command", "budget row",
              "POSIX impact", "rollback threshold")


def cells(line):
    parts = line.strip().strip("|").split("|")
    return [c.strip() for c in parts]


def check(path, coverage=False):
    errors = []
    lines = path.read_text().splitlines()
    header = None
    rows = 0
    layers = set()
    for n, line in enumerate(lines, 1):
        line = line.lstrip()
        if not line.startswith("|"):
            continue
        c = cells(line)
        if c[0].lower() == "id":
            header = [h.lower() for h in c]
            if header[:len(BASE)] != BASE:
                errors.append(f"{path}:{n}: header must start with {BASE}")
                return errors
            continue
        if header is None or set(c[0]) <= set("-: "):
            continue
        rows += 1
        row = dict(zip(header, c))
        rid = row.get("id", "")
        if len(c) != len(header):
            errors.append(f"{path}:{n}: {rid or '?'}: {len(c)} cells, header "
                          f"has {len(header)}")
        for f in BASE:
            if not row.get(f):
                errors.append(f"{path}:{n}: {rid or '?'}: empty field '{f}'")
        if row.get("status") and row["status"] not in STATUS:
            errors.append(f"{path}:{n}: {rid}: status '{row['status']}' not "
                          f"one of {sorted(STATUS)}")
        layer = row.get("layer", "")
        if not layer:
            errors.append(f"{path}:{n}: {rid or '?'}: empty field 'layer'")
        elif layer not in LAYERS:
            errors.append(f"{path}:{n}: {rid}: layer '{layer}' not one of "
                          f"{list(LAYERS)}")
        else:
            layers.add(layer)
        if "platform" in row or coverage:
            plat = row.get("platform", "")
            if not PLATFORM_RE.fullmatch(plat):
                errors.append(f"{path}:{n}: {rid}: platform '{plat}' must be "
                              "'n/a' or '<class> PB-ID[, PB-ID...]'")
        tags = [t for t in TAGS if t in rid]
        if tags:
            tf = row.get("tag-fields", "")
            for f in TAG_FIELDS:
                if f"{f}:" not in tf:
                    errors.append(f"{path}:{n}: {rid}: tag '{tags[0]}' needs "
                                  f"'{f}:' in tag-fields")
    if header is None or rows == 0:
        errors.append(f"{path}: no claim table found")
    if coverage:
        for layer in LAYERS:
            if layer not in layers:
                errors.append(f"{path}: layer '{layer}' has no claim row")
    return errors


def rows_of(path):
    """(id, layer) for every claim row, tolerant of a missing layer column."""
    out, header = [], None
    for line in path.read_text().splitlines():
        line = line.lstrip()
        if not line.startswith("|"):
            continue
        c = cells(line)
        if c[0].lower() == "id":
            header = [h.lower() for h in c]
        elif header and not set(c[0]) <= set("-: "):
            out.append((c[0], dict(zip(header, c)).get("layer", "")))
    return out


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--expect-fail", action="store_true")
    ap.add_argument("--layers", action="store_true")
    ap.add_argument("files", nargs="*")
    args = ap.parse_args(argv)
    default = Path(__file__).resolve().parent.parent / "docs/formal-claims.md"
    files = [Path(a) for a in args.files] or [default]
    rc = 0
    for f in files:
        errs = check(f, coverage=args.layers or f == default)
        if args.expect_fail:
            if not errs:
                print(f"check-formal-claims: {f}: expected a failure, got none",
                      file=sys.stderr)
                rc = 1
        elif errs:
            print("\n".join(errs), file=sys.stderr)
            rc = 1
    if rc == 0:
        print("check-formal-claims: ok")
        if files == [default] and not args.expect_fail:
            rows = rows_of(default)
            per = Counter(layer for _, layer in rows)
            gate_cert.emit(f"{len(rows)} rows have all seven base fields and a valid status")
            for layer in LAYERS:
                ids = [i for i, l in rows if l == layer]
                gate_cert.emit(f"layer {layer}: {per[layer]} rows ({', '.join(ids)})")
            gate_cert.emit(f"{len(rows)} rows valid across {len(LAYERS)} layers", result=True)
    return rc


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
