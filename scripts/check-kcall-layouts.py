#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
#
# Roottask and the kernel each define the argument structs of the private
# kernel calls (syscall_priv): roottask fills one in and the kernel reads it.
# Most are copied by hand on both sides, so a field one side moves is read
# wrong by the other with no warning (kext_thread_ctl_call's commands
# arrived as other commands). This gate takes every struct that both trees
# define under one name and compares the two layouts in the debug info of the
# built kernel and roottask:
#   - the same size and the same top-level field names, in order;
#   - every scalar roottask writes, as (member path, bit offset, bit size), is
#     one the kernel reads (a member of a kernel KARGSLOT union counts; bit
#     fields and array elements are compared too).
# A struct that neither image holds (compiled out in this config) is listed,
# not failed; one that only one image holds fails, as does a run that checks
# fewer than half the structs (a stripped ELF). --self-test checks the
# comparison on fixtures.
#
# Usage: scripts/check-kcall-layouts.py [--gdb GDB] --kernel ELF --roottask ELF
# (GDB: $GDB, else riscv-none-elf-gdb-py3 or gdb-multiarch, with Python)

import argparse
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DUMP = ROOT / "scripts/gdb/kcall-layout.py"
# struct [attributes] NAME {, typedef'd or not, the brace on the same line or
# the next.
ATTR = r"(?:__attribute__\s*\(\(.*?\)\)\s*)*"
STRUCT_RE = re.compile(r"\bstruct\s+" + ATTR + r"([A-Za-z_]\w*)\s*" + ATTR + r"\{")
SLOT_RE = re.compile(r"\._slot\d+$")  # KARGSLOT padding members


def fail(msg):
    print(f"check-kcall-layouts: {msg}", file=sys.stderr)
    sys.exit(1)


def defined(dirs):
    names = set()
    for d in dirs:
        for p in (ROOT / d).rglob("*.[ch]"):
            if p.is_file():
                names.update(STRUCT_RE.findall(p.read_text(errors="replace")))
    return names


def dump(gdb, elves, names):
    """The layouts of names in each of elves, read by gdb runs side by side."""
    env = dict(os.environ, KCALL_STRUCTS=" ".join(sorted(names)))
    try:
        runs = [subprocess.Popen([gdb, "-batch", "-nx", "-ex", f"source {DUMP}", elf],
                                 env=env, stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, text=True)
                for elf in elves]
    except FileNotFoundError:
        fail(f"no {gdb} (set GDB to a gdb with Python for this target)")
    layouts = []
    for elf, run in zip(elves, runs):
        out = run.communicate()[0]
        for line in out.splitlines():
            if line.startswith("KCALLJSON"):
                layouts.append(json.loads(line[len("KCALLJSON"):]))
                break
        else:
            fail(f"{gdb} gave no layouts for {elf} (a gdb with Python is needed)")
    return layouts


def compare(name, rt, k):
    """The differences between roottask's layout rt and the kernel's k."""
    errs = []
    if rt["size"] != k["size"]:
        errs.append(f"size {rt['size']} in roottask, {k['size']} in the kernel")
    if rt["fields"] != k["fields"]:
        errs.append(f"fields {rt['fields']} in roottask, {k['fields']} in the kernel")
    kl = {tuple(x) for x in k["leaves"]}
    rl = {tuple(x) for x in rt["leaves"]}
    kernel_only = sorted(kl - rl)
    for path, bit, bits in sorted(rl):
        if (path, bit, bits) not in kl:
            errs.append(f"roottask writes {path or '(whole)'} as {bits} bits at bit "
                        f"{bit}, which the kernel does not read as such")
            continue
        # A kernel union alternative roottask does not have, wider than what
        # it writes there, reads past it, unless it is a KARGSLOT slot.
        for kpath, kbit, kbits in kernel_only:
            if (kbit == bit and kbits > bits and
                    not SLOT_RE.search(kpath) and not kpath.endswith(("[]", "[elem]")) and
                    kpath.rsplit(".", 1)[0] == path.rsplit(".", 1)[0]):
                errs.append(f"the kernel reads {kpath} as {kbits} bits at bit {bit}, "
                            f"where roottask writes {path} as {bits}")
    return [f"{name}: {e}" for e in errs]


def self_test():
    ok = {"size": 32, "fields": ["prp", "tid", "tctl"],
          "leaves": [[".prp", 0, 64], [".tid", 64, 32], [".tctl.cmd", 128, 32],
                     [".tctl._slot1", 128, 64], [".tctl.data", 192, 64]]}
    flat = {"size": 24, "fields": ["prp", "tid", "cmd", "data"],
            "leaves": [[".prp", 0, 64], [".tid", 64, 32], [".cmd", 96, 32],
                       [".data", 128, 64]]}
    nested = {"size": 32, "fields": ["prp", "tid", "tctl"],
              "leaves": [[".prp", 0, 64], [".tid", 64, 32], [".tctl.cmd", 128, 32],
                         [".tctl.data", 192, 64]]}
    widened = {"size": 32, "fields": ["prp", "tid", "tctl"],
               "leaves": nested["leaves"] + [[".tctl.wide", 128, 64]]}
    swapped = {"size": 32, "fields": ["prp", "tid", "tctl"],
               "leaves": [[".prp", 0, 64], [".tid", 64, 32], [".tctl.data", 128, 64],
                          [".tctl.cmd", 192, 32]]}
    errs = compare("x", nested, ok)
    if errs:
        fail(f"self-test: a matching layout was refused: {errs}")
    if not compare("x", nested, widened):
        fail("self-test: a wider kernel union alternative passed")
    for bad, what in ((flat, "the flattened thread-control layout"),
                      (swapped, "a layout with two members swapped")):
        if not compare("x", bad, ok):
            fail(f"self-test: {what} passed")
    print("check-kcall-layouts: self-test ok")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--self-test", action="store_true")
    ap.add_argument("--gdb", default=os.environ.get("GDB") or
                    shutil.which("riscv-none-elf-gdb-py3") or "gdb-multiarch")
    ap.add_argument("--kernel")
    ap.add_argument("--roottask")
    args = ap.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.kernel or not args.roottask:
        fail("--kernel and --roottask are required")

    names = defined(["servers/roottask"]) & defined(["kernel", "arch"])
    k, rt = dump(args.gdb, [args.kernel, args.roottask], names)

    errs, absent, checked = [], [], 0
    for n in sorted(names):
        r, kk = rt.get(n), k.get(n)
        if r is None and kk is None:
            absent.append(n)  # compiled out of both in this config
        elif r is None or kk is None:
            errs.append(f"{n}: in the {'kernel' if r is None else 'roottask'} image only")
        else:
            checked += 1
            errs += compare(n, r, kk)
    if not names or checked * 2 < len(names):
        errs.append(f"only {checked} of {len(names)} structs had debug info in both "
                    "images (stripped, or the wrong tree?)")
    for e in errs:
        print(f"check-kcall-layouts: {e}", file=sys.stderr)
    if absent:
        print(f"check-kcall-layouts: not in both images in this config: {' '.join(absent)}")
    if errs:
        return 1
    print(f"check-kcall-layouts: ok ({checked} structs)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
