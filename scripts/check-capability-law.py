#!/usr/bin/env python3
"""Check that each capability law has runtime evidence and a formal row."""

from pathlib import Path

import gate_cert

PROPERTIES = (
    "unforgeability",
    "type-discrimination",
    "rights-enforcement",
    "attenuation-only",
    "transfer-without-grant",
    "revoke-before-free",
    "stale-handle-rejection",
    "no-ambient-lookup",
)

ROOT = Path(__file__).resolve().parents[1]


def main() -> int:
    law = (ROOT / "docs/capability-law.md").read_text()
    claims = (ROOT / "docs/formal-claims.md").read_text()
    errors = []

    for prop in PROPERTIES:
        row = f"| {prop} |"
        claim = f"| capability-law-{prop} |"
        if row not in law:
            errors.append(f"docs/capability-law.md: missing {prop}")
        if claim not in claims:
            errors.append(f"docs/formal-claims.md: missing capability-law-{prop}")

    for token in ("make check-cap-namespace", "make check-cap-smt",
                  "make check-capability-abi"):
        if token not in law:
            errors.append(f"docs/capability-law.md: missing {token}")

    if errors:
        for error in errors:
            print(error)
        return 1
    gate_cert.emit(f"{len(PROPERTIES)} capability laws each have a docs/capability-law.md "
                   "row and a capability-law-* formal claim")
    gate_cert.emit("docs/capability-law.md names make check-cap-namespace, "
                   "check-cap-smt and check-capability-abi")
    gate_cert.emit("ok", result=True)
    print("check-capability-law: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
