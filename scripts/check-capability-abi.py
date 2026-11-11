#!/usr/bin/env python3
"""Check the capability ABI contract covers the kernel-call surface."""

from pathlib import Path
import re
import sys

import gate_cert

ROOT = Path(__file__).resolve().parents[1]
DOC = ROOT / "docs" / "capability-abi.md"
CALLS = ROOT / "internal/include/sys/calls.h"


def ksys_names():
    text = CALLS.read_text(encoding="utf-8")
    body = text[text.index("enum {") : text.index("__KSYS_BAD") + len("__KSYS_BAD")]
    return re.findall(r"^\s*(__KSYS_[A-Z0-9_]+)\b", body, re.M)


def main():
    text = DOC.read_text(encoding="utf-8")
    row_names = re.findall(r"^\| `(__KSYS_[A-Z0-9_]+)` \|", text, re.M)
    rows = set(row_names)
    calls = set(ksys_names())

    required_text = [
        "Append-only rule",
        "Capability is authority; namespace is presentation",
        "RTMUX_CAP_TYPE_ENDPOINT",
        "RTMUX_CAP_TYPE_PROCESS",
        "RTMUX_CAP_ENDPOINT_SEND",
        "RTMUX_CAP_PROCESS_MAKE_THREAD",
        "RTMUX_CAP_MEM_MAP_EXEC",
        "RTMUX_CAP_TRANSFER_MAX",
        "revoke before free",
        "No kernel syscall grants authority",
        "W|X",
        "check-cap-namespace",
    ]

    errors = []
    for name in sorted({name for name in row_names if row_names.count(name) > 1}):
        errors.append(f"duplicate syscall row: {name}")
    for name in sorted(calls - rows):
        errors.append(f"missing syscall row: {name}")
    for name in sorted(rows - calls):
        errors.append(f"stale syscall row: {name}")
    for item in required_text:
        if item not in text:
            errors.append(f"missing contract text: {item}")

    if errors:
        for error in errors:
            print(f"check-capability-abi: {error}", file=sys.stderr)
        return 1
    gate_cert.emit(f"{len(calls)} __KSYS_* kernel calls each have exactly one "
                   "docs/capability-abi.md row, with no stale rows")
    gate_cert.emit(f"{len(required_text)} required contract text items are present")
    print("check-capability-abi: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
