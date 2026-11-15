#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Every guest test_gap() must carry a well-formed tracking anchor.

A known gap is a failing case kept visible on purpose. tc_gap() (tests/userland/
outcome.h) only checks that the anchor string is present; this checks that it is a
tracking id of the form T<section>.<n> (for example "T1.4"). With --tracker FILE it
also checks that the id still names an open item in FILE, an item heading of the
form "- [ ] **T<id> ...", so a gap whose item was finished or deleted cannot linger
and hide a regression.

Calls are matched textually, so the name and the anchor must be plain string literals
(not macros or concatenations; a call that is not of that form is an error), and a
commented-out call is still checked.

  check-gap-anchors.py [--root DIR] [--tracker FILE]   check the tree
  check-gap-anchors.py --self-test                     prove the check on a temporary tree
"""

import re
import sys
import tempfile
from pathlib import Path

CALL = re.compile(r'\btest_gap\(\s*"[^"]*"\s*,\s*"([^"]*)"', re.S)
# Every call, but not the definition ("void test_gap(").
ANY_CALL = re.compile(r"(?<!void )\btest_gap\(")
ITEM = re.compile(r"^- \[ \] \*\*(T\d+\.\d+[ab]?) ", re.M)
ANCHOR = re.compile(r"^(T\d+\.\d+[ab]?)$")


def check(root: Path, tracker: Path | None = None) -> list[str]:
    open_items = None
    if tracker is not None:
        open_items = set(ITEM.findall(tracker.read_text(encoding="utf-8")))
    errors = []
    for src in sorted((root / "tests").rglob("*.c")):
        # The host test exercises tc_gap, not real gaps.
        if src.relative_to(root).parts[:2] == ("tests", "host"):
            continue
        text = src.read_text(encoding="utf-8")
        rel = src.relative_to(root)

        def loc(pos):
            return f"{rel}:{text.count(chr(10), 0, pos) + 1}"

        calls = list(CALL.finditer(text))
        literal = {m.start() for m in calls}
        for m in ANY_CALL.finditer(text):
            if m.start() not in literal:
                errors.append(f"{loc(m.start())}: test_gap call without "
                              "literal name and anchor arguments")
        for m in calls:
            anchor = m.group(1)
            where = loc(m.start())
            a = ANCHOR.match(anchor)
            if not a:
                errors.append(f"{where}: gap anchor {anchor!r} is not a tracking id like T1.4")
            elif open_items is not None and a.group(1) not in open_items:
                errors.append(f"{where}: gap anchor {anchor} names no open tracker item")
    return errors


def self_test() -> int:
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        (root / "tests/userland").mkdir(parents=True)
        tracker = root / "tracker.md"
        tracker.write_text("- [ ] **T1.1 · C · P0 · open item (S).**\n")
        src = root / "tests/userland/x.c"
        cases = [
            ('test_gap("a", "T1.1", "E", rc);', None, 0, "well-formed anchor"),
            ('test_gap("a", "T1.1", "E", rc);', tracker, 0, "open anchor"),
            ('test_gap("a", "T9.9", "E", rc);', tracker, 1, "anchor names no open item"),
            ('test_gap("a", "T9.9", "E", rc);', None, 0, "unknown id without a tracker"),
            ('test_gap("a", "known", "E", rc);', None, 1, "malformed anchor"),
            ('test_gap("a", "", "E", rc);', None, 1, "empty anchor"),
            ('test_gap(NAME, "T1.1", "E", rc);', None, 1, "macro name"),
            ('test_gap("a", ANCHOR, "E", rc);', None, 1, "macro anchor"),
            ('test_gap("a" "b", "T1.1", "E", rc);', None, 1, "concatenated name"),
        ]
        for body, trk, want, what in cases:
            src.write_text(body + "\n")
            got = 1 if check(root, trk) else 0
            if got != want:
                print(f"check-gap-anchors: self-test failed: {what}")
                return 1
    print("check-gap-anchors: self-test ok")
    return 0


def main(argv: list[str]) -> int:
    if argv[1:] == ["--self-test"]:
        return self_test()
    root = Path(__file__).resolve().parents[1]
    tracker = None
    args = argv[1:]
    while args:
        if args[0] == "--root" and len(args) >= 2:
            root = Path(args[1]).resolve()
        elif args[0] == "--tracker" and len(args) >= 2:
            tracker = Path(args[1]).resolve()
        else:
            print(__doc__)
            return 2
        args = args[2:]
    errors = check(root, tracker)
    for e in errors:
        print(f"check-gap-anchors: {e}")
    if errors:
        return 1
    print("check-gap-anchors: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
