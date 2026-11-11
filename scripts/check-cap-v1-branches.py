#!/usr/bin/env python3
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
"""Spectre-V1 branch-inventory regression gate for the IPC capability-use window.

This is NOT a proof that the authority path is branchless.  It is not, and the
gate refuses to pretend otherwise.  The hot authority validator
cap_lookup_pin_typed (kernel/cap/table.c), reached by
rtmux_cap_lookup_pin_context, compares index against table->nslots and only then
loads table->slots[index]; that magnitude compare is a classic Spectre-V1
bounds-check-bypass surface, which RTMux deliberately leaves unmitigated as
a named gap, not silent and not speculatively fixed with a fence or index
mask.

What the gate does enforce: it freezes the ordered conditional-branch signature
of each audited authority window.  A fast-path edit that adds a NEW
data-dependent branch to these windows, for example a second bounds check, a
signedness test, or a secret-dependent branch inserted between capability
validation and the slot->object load, or that reorders the existing branches,
changes the signature and fails the gate.  The reviewer must then regenerate the
golden with --emit and re-classify every branch in docs/smp-contract.md under
"IPC capability-use window".  The signature is ordered, not a bare count, so a
same-mnemonic swap that leaves the count unchanged is still caught.  The single
accepted bounds compare per window is recorded in the doc as the one known V1
gap.

Audited windows (see docs/smp-contract.md for the per-branch classification and
the coverage boundary):
  - cap_lookup_pin_typed: the shared capability authority validator on the IPC
    send fast path.  Its lone bgeu is the accepted V1 bounds check.
  - vector_lookup2: the post-cap object lookup the caller uses to resolve the
    validated context into a connect_t.  Its lone bgeu is likewise accepted.

These two standalone callees hold every mistrainable magnitude compare on the
send authority path.  The caller-side glue in inline_lookup_connect
(kernel/ipc/fastmsg.c) is static inline with no standalone symbol; its branches
(generation discriminator, vector select, cop != object recheck) are inlined
into ksys_msg_sendv and are not frozen here.  That glue carries only equality
and select branches, no bounds compare, so no V1 surface today; the boundary is
documented in docs/smp-contract.md rather than covered, because freezing it
would mean auditing the whole noisy ksys_msg_sendv body.

The golden signature is calibrated to the make defconfig build with the
in-tree riscv-none-elf toolchain.  A deliberate toolchain or config change that
alters codegen is a review event: rerun with --emit, confirm no new mistrainable
branch appeared, and commit the refreshed golden together with the doc update.

Exit status: non-zero on any signature mismatch or a missing/unreadable object.
"""

import os
import re
import subprocess
import sys
from typing import NamedTuple

# Conditional-branch mnemonics.  The magnitude compares in BOUNDS_COMPARE are the
# mistrainable bounds-check class (index < N style); the rest are equality or
# zero tests kept in the signature so it captures the whole branch sequence.
BOUNDS_COMPARE = {
    "blt", "bge", "bltu", "bgeu", "bgt", "ble", "bgtu", "bleu",
    "blez", "bgez", "bltz", "bgtz",
}
EQUALITY_BRANCH = {"beq", "bne", "beqz", "bnez", "c.beqz", "c.bnez"}
BRANCH_OPCODES = BOUNDS_COMPARE | EQUALITY_BRANCH


class Window(NamedTuple):
    obj: str
    symbol: str
    golden: list  # ordered branch mnemonics, in body order


# Frozen signatures.  The golden is the ordered conditional-branch sequence of
# each window, not just a count: an edit that swaps one branch for another same
# mnemonic, or moves the accepted bounds compare, changes the order and fails.
# Regenerate with --emit; see the module docstring.
AUDIT = [
    Window("kernel/cap/table.o", "cap_lookup_pin_typed",
           ["beqz", "bnez", "beqz", "bgeu", "beqz", "beqz", "beqz", "beqz",
            "beq", "bne", "bne", "bnez", "beq", "beqz"]),
    Window("kernel/object/vector.o", "vector_lookup2",
           ["beqz", "bgeu", "bnez"]),
]

TOP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Symbol header line: "000000000000abcd <name>:".  A local label starts with
# ".L"; anything else is a real symbol and ends the current function body.
SYM_RE = re.compile(r"^[0-9a-f]+ <([^>]+)>:")
INSN_RE = re.compile(r"^\s+[0-9a-f]+:\t[0-9a-f ]+\t(\S+)")


def objdump_cmd():
    override = os.environ.get("OBJDUMP")
    if override:
        return override
    cross = os.environ.get("CROSS_COMPILE", "riscv-none-elf-")
    return cross + "objdump"


def function_body(disasm, symbol):
    """Yield mnemonic strings for every instruction inside the named symbol."""
    inside = False
    for line in disasm.splitlines():
        m = SYM_RE.match(line)
        if m:
            name = m.group(1)
            if name == symbol:
                inside = True
                continue
            if inside and not name.startswith(".L"):
                break  # next real symbol ends the body
            continue
        if inside:
            insn = INSN_RE.match(line)
            if insn:
                yield insn.group(1)


def inventory(objdump, obj_path, symbol):
    try:
        disasm = subprocess.check_output([objdump, "-d", obj_path],
                                         text=True, stderr=subprocess.STDOUT)
    except FileNotFoundError:
        sys.exit("error: objdump '%s' not found (set OBJDUMP or CROSS_COMPILE)"
                 % objdump)
    except subprocess.CalledProcessError as exc:
        sys.exit("error: objdump failed on %s:\n%s" % (obj_path, exc.output))
    branches = []
    seen_symbol = False
    for mnem in function_body(disasm, symbol):
        seen_symbol = True
        if mnem in BRANCH_OPCODES:
            branches.append(mnem)
    if not seen_symbol:
        sys.exit("error: symbol '%s' not found in %s (build stale? run make)"
                 % (symbol, obj_path))
    return branches


def fmt(branches):
    return " ".join(branches) if branches else "(none)"


def bounds_count(branches):
    return sum(1 for b in branches if b in BOUNDS_COMPARE)


def main():
    emit = "--emit" in sys.argv[1:]
    objdump = objdump_cmd()
    out = os.environ.get("OUT", "build")
    failed = False

    for w in AUDIT:
        obj_path = os.path.join(TOP, out, w.obj)
        if not os.path.exists(obj_path):
            sys.exit("error: %s missing; build the kernel first (make)" % obj_path)
        got = inventory(objdump, obj_path, w.symbol)

        if emit:
            body = ", ".join('"%s"' % b for b in got)
            print('    Window("%s", "%s",\n           [%s]),'
                  % (w.obj, w.symbol, body))
            continue

        if got != w.golden:
            failed = True
            print("FAIL %s (%s)" % (w.symbol, w.obj))
            print("  golden: %s" % fmt(w.golden))
            print("  actual: %s" % fmt(got))
            if bounds_count(got) > bounds_count(w.golden):
                print("  NEW bounds-compare branch: a mistrainable Spectre-V1"
                      " window may have been reintroduced. Justify and document"
                      " in docs/smp-contract.md before regenerating the golden.")
            else:
                print("  branch signature changed. If intentional, re-verify no"
                      " new mistrainable branch sits between cap validation and"
                      " the slot->object load, update docs/smp-contract.md, and"
                      " regenerate with --emit.")
            continue

        print("ok   %s: %s (accepted bounds compares: %d)"
              % (w.symbol, fmt(got), bounds_count(got)))

    if failed:
        sys.exit(1)
    if not emit:
        print("cap-v1 branch inventory frozen; no new data-dependent branch in"
              " the audited capability-use windows.")


if __name__ == "__main__":
    main()
