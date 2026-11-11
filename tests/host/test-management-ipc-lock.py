#!/usr/bin/env python3
"""Cheap guard for the section 7 management-IPC lock rule."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / "docs" / "smp-contract.md"

doc = DOC.read_text(encoding="utf-8")
for needle in [
    "## Management IPC Lock State",
    "No synchronous management IPC while holding roottask object locks.",
    "`path_mutex`",
    "`coresrv -> namesrv`",
]:
    if needle not in doc:
        print(f"smp contract missing: {needle}", file=sys.stderr)
        raise SystemExit(1)

blocking = re.compile(
    r"\b(msg_send|msg_sendnc|msg_sendv|msg_sendvnc|open|sopen|_connect|name_open)\s*\("
)
lock_start = re.compile(
    r"\b(path_node_access\s*\(|pthread_mutex_lock\s*\(\s*&path_mutex\s*\))"
)
lock_end = re.compile(
    r"\b(path_node_complete\s*\(|pthread_mutex_unlock\s*\(\s*&path_mutex\s*\))"
)

failures = []
for path in sorted((ROOT / "roottask").rglob("*.c")):
    held = False
    start = 0
    for lineno, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if lock_start.search(line):
            held = True
            start = lineno
        if held and blocking.search(line):
            rel = path.relative_to(ROOT)
            failures.append(
                f"{rel}:{lineno}: blocking call while namespace lock held "
                f"from line {start}"
            )
        if held and lock_end.search(line):
            held = False

if failures:
    print("\n".join(failures), file=sys.stderr)
    raise SystemExit(1)

print("management-ipc-lock-test: ok")
