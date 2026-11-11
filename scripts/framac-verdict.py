#!/usr/bin/env python3
"""Decide whether a Frama-C WP run counts as a proof, and that it proved everything.

Frama-C's own "Proved goals: N / M" line is not evidence enough on its own.
Two failure modes pass it: a run that crashed after printing a summary, and a
run where M is small because the body or the contract went away. Both report
success. So four independent gates:

  1. The process exited 0.
  2. No "User Error" appeared, which means the input was rejected.
  3. At least --min-goals obligations were GENERATED. Gutting a function body
     proves 0 of 0, and that is not a proof.
  4. Every generated obligation was discharged.

Plus a fifth check that is not about the log at all. Frama-C ASSUMES the
contract of any function it was not asked to prove, so a contracted function
left out of -wp-fct becomes an unchecked axiom that every goal above it rests
on, and the run still reports success. --fcts and --scan compare the proof set
against the contracts actually present in the source.

Both ideas are lifted from ../elfuse, which learned them the hard way: it has
a recorded case where replacing a contracted helper's body with a constant
that contradicted its own ensures still produced "PROVED 74 of 74".

Usage:
    framac-verdict.py --log FILE --status N --min-goals N [--src FILE]
                      [--fcts "a b c"] [--scan FILE ...]
"""

import argparse
import re
import sys

# An obligation WP could not discharge.  The memory-model prefix on a goal name
# varies per run and says nothing about which function is open, so strip it.
OPEN_GOAL = re.compile(
    r"^\[wp\] \[(?:Timeout|Stepout|Unknown|Failed)\] "
    r"(?:typed_nocast_|typed_caveat_|typed_|bytes_)?([A-Za-z0-9_]+)"
)
PROVED_GOALS = re.compile(r"^\[wp\] Proved goals: *([0-9]+) */ *([0-9]+)")
USER_ERROR = re.compile(r"^.*User Error: *")
# A function WP took on faith: no body and no contract it could analyze.
ASSUMED = re.compile(
    r"Neither code nor explicit .* for function ([A-Za-z_][A-Za-z_0-9]*),"
)

# ACSL annotations, both the block and the one-line form.  Scanning only /*@
# would let two characters reopen the hole this check exists to close.
ACSL_BLOCK = re.compile(
    r"/\*@.*?\*/|^[ \t]*//@[^\n]*", re.DOTALL | re.MULTILINE
)
# Annotations with no function to attribute them to: they sit inside a body
# that its own function's contract already covers, or they declare logic.
NOT_A_CONTRACT = re.compile(
    r"^\s*(predicate|logic|axiomatic|lemma|type|ghost"
    r"|loop|assert|check|admit|assume|breaks|continues|returns)\b"
)
# The optional parentheses around the name are not decoration: the tree writes
# "int(xfer_memchk)(...)" to keep a same-named macro from expanding, and a
# regex that does not expect it reports the contract as unattributable.
DEFINITION = re.compile(
    r"""^[ \t]*
        (?:static\s+|inline\s+)*
        [A-Za-z_][A-Za-z_0-9\s*]*?
        (?:\(\s*)?
        \b(?P<name>[A-Za-z_][A-Za-z_0-9]*)\s*
        (?:\)\s*)?
        \([^;{]*?\)\s*
        \{
    """,
    re.VERBOSE | re.DOTALL,
)


def contracted(path):
    """Function names carrying a contract in @path, and contracts with no owner.

    An annotation this cannot attribute to a function is reported rather than
    skipped. Silently ignoring a contract is the exact failure being guarded
    against, so it has to be loud.
    """
    text = open(path, encoding="utf-8").read()
    names, orphans = [], []
    for block in ACSL_BLOCK.finditer(text):
        raw = block.group(0)
        body = raw[3:-2] if raw.startswith("/*") else re.sub(r"^[ \t]*//@?", "", raw)
        if NOT_A_CONTRACT.match(body.lstrip("\n")):
            continue
        rest = text[block.end():]
        # Step over comments and preprocessor lines between the annotation and
        # the definition it belongs to; either one defeats a bare regex.
        while True:
            stripped = rest.lstrip()
            if stripped.startswith("/*"):
                rest = stripped[stripped.index("*/") + 2:]
            elif stripped.startswith("#"):
                rest = stripped[stripped.index("\n") + 1:] if "\n" in stripped else ""
            else:
                rest = stripped
                break
        match = DEFINITION.match(rest)
        if match:
            names.append(match.group("name"))
        else:
            orphans.append(raw.splitlines()[0].strip()[:70])
    return names, orphans


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", required=True)
    ap.add_argument("--status", type=int, required=True)
    ap.add_argument("--min-goals", type=int, default=1)
    ap.add_argument("--src", default="")
    ap.add_argument("--fcts", default="")
    ap.add_argument("--assume", default="")
    ap.add_argument("--scan", nargs="*", default=[])
    args = ap.parse_args()

    log = open(args.log, encoding="utf-8", errors="replace").read()
    lines = log.splitlines()
    failures = []

    proof_set = set(re.split(r"[,\s]+", args.fcts.strip())) - {""}
    # Contracts deliberately taken as axioms: a body no prover can look inside,
    # such as inline asm.  They are excluded from the coverage failure and
    # printed on every run instead, because an assumption nobody sees is
    # indistinguishable from a proof nobody checked.
    assumed_ok = set(re.split(r"[,\s]+", args.assume.strip())) - {""}
    proof_set |= assumed_ok
    for path in args.scan:
        names, orphans = contracted(path)
        missing = [n for n in names if n not in proof_set]
        if missing:
            failures.append(
                f"{path}: contracted but not in the proof set: "
                + ", ".join(sorted(set(missing)))
                + "\n    WP assumes the contract of any function it is not asked"
                " to prove, so each of these is an unchecked axiom"
            )
        if orphans:
            failures.append(
                f"{path}: ACSL this check cannot attribute to a function: "
                + "; ".join(orphans)
            )

    if args.status != 0:
        failures.append(f"frama-c exited {args.status}")

    errors = [USER_ERROR.sub("", ln) for ln in lines if "User Error" in ln]
    if errors:
        failures.append("frama-c rejected the input: " + errors[0].strip())

    assumed = sorted({m.group(1) for ln in lines for m in [ASSUMED.search(ln)] if m})
    if assumed:
        failures.append(
            "WP had neither body nor contract for: " + ", ".join(assumed)
            + "\n    these were taken on faith, so the result rests on them"
        )

    summary = [PROVED_GOALS.match(ln) for ln in lines]
    summary = [m for m in summary if m]
    if not summary:
        failures.append("no 'Proved goals' summary in the log")
        proved = total = 0
    else:
        proved, total = int(summary[-1].group(1)), int(summary[-1].group(2))
        if total < args.min_goals:
            failures.append(
                f"only {total} obligations generated, expected at least"
                f" {args.min_goals}\n    a gutted body or a dropped contract"
                " proves trivially; lower the floor deliberately or restore"
                " what went missing"
            )
        if proved != total:
            open_goals = sorted({
                m.group(1) for ln in lines for m in [OPEN_GOAL.match(ln)] if m
            })
            failures.append(
                f"{proved}/{total} discharged; open: " + ", ".join(open_goals)
            )

    where = args.src or args.log
    if assumed_ok and not failures:
        print(f"verify: {where} ASSUMES (axiom, not proved): "
              + ", ".join(sorted(assumed_ok)))
    if failures:
        print(f"verify: {where} FAILED", file=sys.stderr)
        for f in failures:
            print(f"  {f}", file=sys.stderr)
        return 1
    print(f"verify: {where} {proved}/{total} goals valid [{args.fcts}]")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
