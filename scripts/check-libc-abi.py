#!/usr/bin/env python3
#
# Gate the dynamic export surface of libc.so against docs/abi/libc.exports and
# lib/libc/libc.map.  Each exported symbol is normalized to
# "name type bind vis version" (tab separated, sorted by name).
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
EXPORTS = ROOT / "docs/abi/libc.exports"
MAP = ROOT / "lib/libc/libc.map"
TESTDATA = ROOT / "scripts/testdata/libc-abi"
BASE = "ok.exports"
FIXTURES = ("extra", "missing", "changed")


def fail(msg):
    print(f"check-libc-abi: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_exports(text, origin):
    """Return {name: (type, bind, vis, version)} from an exports file."""
    syms = {}
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line.strip() or line.startswith("#"):
            continue
        f = line.split()
        if len(f) != 5:
            fail(f"{origin}:{lineno}: expected 5 fields, got {len(f)}")
        if f[0] in syms:
            fail(f"{origin}:{lineno}: duplicate symbol {f[0]}")
        syms[f[0]] = tuple(f[1:])
    return syms


def read_so(so, readelf):
    """Normalize the defined dynamic symbols of SO."""
    out = subprocess.run([readelf, "--dyn-syms", "-W", so], check=True,
                         capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        # Num: Value Size Type Bind Vis Ndx Name; drop annotations such as [VARIANT_CC].
        f = [t for t in line.split() if not t.startswith("[")]
        if len(f) < 8 or not f[0][:-1].isdigit() or f[6] == "UND":
            continue
        name, _, ver = f[7].partition("@")
        syms[name] = (f[3], f[4], f[5], ver.lstrip("@") or "-")
    return syms


def map_globals(text):
    """Return the global names of an anonymous-node version script and
    whether it ends with a catch-all `local: *;`."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    m = re.search(r"\{(.*)\}\s*;", text, re.S)
    if not m:
        fail("libc.map: no version node")
    glob, _, loc = m.group(1).partition("local:")
    names = [n.strip() for n in glob.replace("global:", "").split(";") if n.strip()]
    return names, loc.strip() == "*;"


def compare(want, got):
    """Return the differences between baseline WANT and actual GOT."""
    errs = []
    for n in sorted(got.keys() - want.keys()):
        errs.append(f"extra export {n} {' '.join(got[n])}")
    for n in sorted(want.keys() - got.keys()):
        errs.append(f"missing export {n}")
    for n in sorted(want.keys() & got.keys()):
        if want[n] != got[n]:
            errs.append(f"changed export {n}: {' '.join(want[n])} -> {' '.join(got[n])}")
    return errs


def self_test():
    base = parse_exports((TESTDATA / BASE).read_text(), BASE)
    if compare(base, base):
        fail(f"self-test: {BASE} does not match itself")
    for kind in FIXTURES:
        path = TESTDATA / f"{kind}.exports"
        errs = compare(base, parse_exports(path.read_text(), path.name))
        if not errs or not all(e.startswith(kind) for e in errs):
            fail(f"self-test: {path.name} should report only '{kind}', got {errs}")
    print("check-libc-abi: self-test ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--so", help="libc.so to inspect")
    ap.add_argument("--readelf", default=os.environ.get("READELF", "readelf"))
    ap.add_argument("--exports", default=str(EXPORTS))
    ap.add_argument("--map", default=str(MAP))
    ap.add_argument("--update", action="store_true",
                    help="print the exports of --so instead of checking")
    args = ap.parse_args()

    if args.self_test:
        self_test()
        return 0
    if not args.so:
        fail("--so is required")

    got = read_so(args.so, args.readelf)
    if args.update:
        for n in sorted(got):
            print(n, *got[n], sep="\t")
        return 0

    want = parse_exports(Path(args.exports).read_text(), args.exports)
    errs = compare(want, got)

    names, catch_all = map_globals(Path(args.map).read_text())
    if not catch_all:
        errs.append("libc.map: missing `local: *;`")
    if len(names) != len(set(names)):
        errs.append("libc.map: duplicate global entries")
    for n in sorted(set(names) ^ want.keys()):
        errs.append(f"libc.map and libc.exports disagree on {n}")

    if errs:
        for e in errs:
            print(f"check-libc-abi: {e}", file=sys.stderr)
        return 1
    print(f"check-libc-abi: ok ({len(got)} exports)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
