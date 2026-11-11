#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

python3 - "$ROOT" "$@" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
args = sys.argv[2:]
expect_fail = False
if args and args[0] == "--expect-fail":
    expect_fail = True
    args = args[1:]

doc = root / (args[0] if args else "docs/fault-recovery.md")
required_sections = (
    "Event Record",
    "Recovery Rules",
    "Taxonomy",
    "Claim Limits",
)
required_events = {
    "illegal-syscall",
    "invalid-capability",
    "user-page-fault",
    "watchdog-expiry",
    "owner-death",
    "server-crash",
}
valid_severity = {"deny", "thread-fatal", "service-fatal", "system-fatal"}
valid_status = {"specified", "tested"}
path_ref = re.compile(r"`((?:docs|scripts|tests|tools|kernel|servers|userland)/[^`]+)`")


def section_exists(text, name):
    return re.search(rf"^## {re.escape(name)}\s*$", text, re.M) is not None


def taxonomy_rows(text):
    rows = []
    in_taxonomy = False
    for line_no, line in enumerate(text.splitlines(), 1):
        if line == "## Taxonomy":
            in_taxonomy = True
            continue
        if in_taxonomy and line.startswith("## "):
            break
        if not in_taxonomy or not line.startswith("|") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0] == "event id":
            continue
        rows.append((line_no, cells))
    return rows


def check():
    errors = []
    text = doc.read_text(encoding="utf-8")

    for section in required_sections:
        if not section_exists(text, section):
            errors.append(f"{doc}: missing section `{section}`")

    seen = set()
    for line_no, cells in taxonomy_rows(text):
        if len(cells) != 11:
            errors.append(f"{doc}: line {line_no}: expected 11 taxonomy columns")
            continue
        event, severity, trigger, action, notify, recovery, log_fields, alloc, authority, evidence, status = cells
        seen.add(event)
        for label, value in (
            ("trigger", trigger),
            ("kernel action", action),
            ("supervisor notification", notify),
            ("recovery option", recovery),
            ("immutable log fields", log_fields),
            ("allocation rule", alloc),
            ("authority delta", authority),
            ("evidence", evidence),
        ):
            if not value or value.upper() in {"TODO", "TBD", "N/A"}:
                errors.append(f"{doc}: line {line_no}: empty {label}")
        if severity not in valid_severity:
            errors.append(f"{doc}: line {line_no}: invalid severity `{severity}`")
        if status not in valid_status:
            errors.append(f"{doc}: line {line_no}: invalid status `{status}`")
        if status == "tested" and "make " not in evidence and "scripts/" not in evidence:
            errors.append(f"{doc}: line {line_no}: tested row lacks executable evidence")
        if "allocates" in alloc.lower():
            errors.append(f"{doc}: line {line_no}: allocation rule permits allocation")
        if authority != "none":
            errors.append(f"{doc}: line {line_no}: authority delta must be `none`")
        for ref in path_ref.findall("; ".join(cells)):
            path = root / ref.split("#", 1)[0]
            if not path.exists():
                errors.append(f"{doc}: line {line_no}: missing referenced artifact `{ref}`")

    for event in sorted(required_events - seen):
        errors.append(f"{doc}: taxonomy lacks `{event}` row")
    if not seen:
        errors.append(f"{doc}: taxonomy has no rows")
    return errors


errors = check()
if expect_fail:
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        raise SystemExit(0)
    print(f"{doc}: negative fixture unexpectedly passed", file=sys.stderr)
    raise SystemExit(1)

for error in errors:
    print(error, file=sys.stderr)
raise SystemExit(1 if errors else 0)
PY
