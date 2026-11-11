#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

python3 - "$ROOT" "$@" <<'PY'
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
sys.path.insert(0, str(root / "scripts"))
import gate_cert  # noqa: E402
args = sys.argv[2:]
expect_fail = False
if args and args[0] == "--expect-fail":
    expect_fail = True
    args = args[1:]

doc = root / (args[0] if args else "docs/safety-runtime.md")
formal = root / "docs/formal-claims.md"

required_sections = (
    "TCB Boundary",
    "Assumptions Of Use",
    "Hazards",
    "Non-Goals",
    "Claim Limits",
    "Residual Risks",
    "Trace Matrix",
    "Limitations",
)
required_classes = {
    "strong-isolation",
    "deterministic-execution",
    "controlled-recovery",
    "safety-runtime",
}
claim_words = re.compile(
    r"\b(certify|certified|certification|certification-ready|certifiable|ASIL|SIL|compliant)\b",
    re.I,
)
path_ref = re.compile(r"`((?:docs|scripts|tests|tools|kernel|servers|userland)/[^`]+)`")


def section_bounds(text, name):
    match = re.search(rf"^## {re.escape(name)}\s*$", text, re.M)
    if not match:
        return None
    next_match = re.search(r"^## ", text[match.end():], re.M)
    end = match.end() + next_match.start() if next_match else len(text)
    return match.start(), end


def table_rows(text):
    rows = []
    in_matrix = False
    for line_no, line in enumerate(text.splitlines(), 1):
        if line == "## Trace Matrix":
            in_matrix = True
            continue
        if in_matrix and line.startswith("## "):
            break
        if not in_matrix or not line.startswith("|") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0] == "claim id":
            continue
        rows.append((line_no, cells))
    return rows


def formal_ids():
    ids = set()
    for line in formal.read_text(encoding="utf-8").splitlines():
        if not line.startswith("|") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0] != "id":
            ids.add(cells[0])
    return ids


def formal_rows():
    rows = {}
    for line_no, line in enumerate(formal.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("|") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0] != "id":
            rows[cells[0]] = (line_no, cells)
    return rows


def has_backtick_token(text, token):
    return f"`{token}`" in text


def check():
    errors = []
    text = doc.read_text(encoding="utf-8")

    for section in required_sections:
        if section_bounds(text, section) is None:
            errors.append(f"{doc}: missing section `{section}`")

    limits = section_bounds(text, "Limitations")
    for match in claim_words.finditer(text):
        if limits and limits[0] <= match.start() < limits[1]:
            continue
        line = text[:match.start()].count("\n") + 1
        errors.append(f"{doc}: line {line}: certification-status wording outside Limitations")

    rows = table_rows(text)
    seen_ids = set()
    seen_classes = set()
    for line_no, cells in rows:
        if len(cells) != 7:
            errors.append(f"{doc}: line {line_no}: expected 7 trace columns")
            continue
        claim_id, claim_class, boundary, assumption, design, evidence, risk = cells
        seen_ids.add(claim_id)
        seen_classes.add(claim_class)
        for label, value in (
            ("boundary", boundary),
            ("assumption boundary", assumption),
            ("design note", design),
            ("evidence artifact", evidence),
            ("residual risk", risk),
        ):
            if not value or value.upper() in {"TODO", "TBD", "N/A"}:
                errors.append(f"{doc}: line {line_no}: empty {label}")
        if claim_class in required_classes and "`" not in evidence and "make " not in evidence:
            errors.append(f"{doc}: line {line_no}: evidence lacks artifact or command")
        for ref in path_ref.findall("; ".join(cells)):
            stats["refs"] += 1
            path = root / ref.split("#", 1)[0]
            if not path.exists():
                errors.append(f"{doc}: line {line_no}: missing referenced artifact `{ref}`")

    missing_classes = required_classes - seen_classes
    for claim_class in sorted(missing_classes):
        errors.append(f"{doc}: trace matrix lacks `{claim_class}` row")

    stats["rows"] = len(rows)
    stats["classes"] = len(seen_classes)
    stats["formal"] = len(formal_ids())
    missing_formal = formal_ids() - seen_ids
    for claim_id in sorted(missing_formal):
        errors.append(f"{doc}: formal claim `{claim_id}` is not referenced")

    deterministic = formal_rows().get("deterministic-baseline-runtime")
    if deterministic is None:
        errors.append(f"{formal}: missing `deterministic-baseline-runtime` row")
    else:
        line_no, cells = deterministic
        row_text = " ".join(cells)
        for command in (
            "make check-rt-budgets",
            "make check-trace-export",
            "make footprint-report",
        ):
            if not has_backtick_token(row_text, command):
                errors.append(
                    f"{formal}: line {line_no}: deterministic row lacks `{command}`"
                )
        for token in (
            "docs/rt-budgets.md",
            "config-hash",
            "max-ns",
            "p99-ns",
            "histogram-artifact",
            "footprint",
        ):
            if token not in row_text:
                errors.append(
                    f"{formal}: line {line_no}: deterministic row lacks `{token}`"
                )

    if not rows:
        errors.append(f"{doc}: trace matrix has no rows")
    return errors


stats = {"refs": 0, "rows": 0, "classes": 0, "formal": 0}
errors = check()
name = doc.relative_to(root) if doc.is_relative_to(root) else doc
if expect_fail:
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        gate_cert.emit(f"{name} is rejected with {len(errors)} errors (negative fixture)")
        raise SystemExit(0)
    print(f"{doc}: negative fixture unexpectedly passed", file=sys.stderr)
    raise SystemExit(1)

for error in errors:
    print(error, file=sys.stderr)
if not errors:
    gate_cert.emit(f"{name} has all {len(required_sections)} required sections and "
                   f"{stats['classes']} trace-matrix classes over {stats['rows']} rows")
    gate_cert.emit(f"all {stats['formal']} docs/formal-claims.md rows are referenced by "
                   "the trace matrix")
    gate_cert.emit(f"all {stats['refs']} artifact paths cited by the trace matrix exist")
    gate_cert.emit("ok; negative fixture rejected", result=True)
raise SystemExit(1 if errors else 0)
PY
