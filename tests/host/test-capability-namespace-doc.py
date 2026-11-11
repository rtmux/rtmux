#!/usr/bin/env python3
"""Small contract check for docs/capability-namespace-feasibility.md."""

from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
DOC = ROOT / "docs" / "capability-namespace-feasibility.md"

text = DOC.read_text(encoding="utf-8")

required = [
    "Capability is authority.  Namespace is presentation.  File protocol is operation.",
    "tProcess.priv_caps",
    "old broad flag",
    "kernel/cap/table.c",
    "`uint64_t`",
    "`uint32_t`",
    "namespace version",
    "`coid`",
    "`fd`",
    "`rcvid`",
    "`/proc/<pid>/wait`",
    "`/proc/<pid>/fd/<n>`",
    "`docs/channel-registry-design.md`",
    "No `int`-on-pointer",
    "No RTMux document may claim capability security",
]

missing = [item for item in required if item not in text]
if missing:
    for item in missing:
        print(f"capability namespace doc missing: {item}", file=sys.stderr)
    raise SystemExit(1)

print("capability-namespace-doc-test: ok")
