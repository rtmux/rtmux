#!/usr/bin/env python3
import io
import os
import re
import sys
from pathlib import Path


REQUIRED = [
    "class",
    "command",
    "config-hash",
    "target",
    "platform",
    "opensbi",
    "kernel-git-sha",
    "samples",
    "max-ns",
    "p99-ns",
    "histogram-artifact",
]

# `platform: <qemu-only|fixture|production> PB-ID[, PB-ID...]`; the ids are
# resolved by scripts/check-platform-boundary-contracts.sh.
# The pattern is shared through scripts/platform-field.ere.
with open(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                       "platform-field.ere")) as _f:
    PLATFORM_RE = re.compile(_f.read().strip())

PERF_REQUIRED = [
    "claim",
    "metric",
    "command",
    "config",
    "baseline",
    "result",
    "rollback-threshold",
]

PERF_CLAIM_RE = re.compile(
    r"\b("
    r"lowers? latency|higher throughput|smaller footprint|better scalability|"
    r"faster fast path|reduces? footprint|improves? throughput|scalability win"
    r")\b",
    re.I,
)
PERF_CLAIM_CONTEXT_LINES = 12


def parse_block(text, start_line):
    data = {}
    for off, line in enumerate(text.splitlines(), start_line):
        if not line.strip():
            continue
        if ":" not in line:
            raise ValueError(f"line {off}: expected key: value")
        key, value = line.split(":", 1)
        data[key.strip()] = value.strip()
    return data


def main(argv):
    schema_only = False
    expect_fail = False
    expect_text = None
    args = argv[1:]
    if args and args[0] == "--expect-fail":
        expect_fail = True
        args = args[1:]
        # the failure must be the named one, so a fixture cannot pass for an
        # unrelated reason
        if args and args[0] == "--expect-error":
            expect_text = args[1]
            args = args[2:]
    if args and args[0] == "--schema-only":
        schema_only = True
        args = args[1:]
    if len(args) != 1:
        print("usage: rt-budgets-check.py [--schema-only] docs/rt-budgets.md",
              file=sys.stderr)
        return 2
    path = Path(args[0])
    # read before capturing stderr so an unreadable file reports its error
    text = path.read_text(encoding="utf-8")
    real_stderr = sys.stderr
    if expect_text is not None:
        sys.stderr = io.StringIO()
    rc = 0
    blocks = list(re.finditer(r"^```rt-budget\n(.*?)^```", text, re.M | re.S))
    for block in blocks:
        start_line = text[:block.start(1)].count("\n") + 1
        try:
            data = parse_block(block.group(1), start_line)
        except ValueError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            rc = 1
            continue
        for key in REQUIRED:
            if key not in data or not data[key]:
                print(f"{path}: line {start_line}: missing `{key}`", file=sys.stderr)
                rc = 1
        if data.get("platform") and not PLATFORM_RE.fullmatch(data["platform"]):
            print(f"{path}: line {start_line}: malformed `platform` "
                  "(want `<class> PB-ID[, PB-ID...]`)", file=sys.stderr)
            rc = 1
        platform_ids = re.findall(r"PB-[A-Z0-9-]+", data.get("platform", ""))
        iommu = [i for i in platform_ids if i.startswith("PB-IOMMU-")]
        # Row-class mapping: only LKL network rows rest on an IOMMU contract, and
        # each cites exactly one of the described/undescribed stream contracts.
        is_net = data.get("class", "").startswith("LKL network")
        if iommu and not is_net:
            print(f"{path}: line {start_line}: only an `LKL network` class may "
                  f"cite {iommu[0]}", file=sys.stderr)
            rc = 1
        if is_net and sorted(set(iommu)) not in (["PB-IOMMU-ABSENT"],
                                                  ["PB-IOMMU-UNDESCRIBED"]):
            print(f"{path}: line {start_line}: an `LKL network` class must cite "
                  "exactly one of PB-IOMMU-ABSENT, PB-IOMMU-UNDESCRIBED",
                  file=sys.stderr)
            rc = 1
        if "date" in data and not re.match(r"^\d{4}-\d{2}-\d{2}$", data["date"]):
            print(f"{path}: line {start_line}: date must be YYYY-MM-DD",
                  file=sys.stderr)
            rc = 1
        for key in ("samples", "max-ns", "p99-ns"):
            if key in data and not re.match(r"^\d+$", data[key]):
                print(f"{path}: line {start_line}: `{key}` must be integer",
                      file=sys.stderr)
                rc = 1
        if "budget-ns" in data:
            if not re.match(r"^\d+$", data["budget-ns"]):
                print(f"{path}: line {start_line}: `budget-ns` must be integer",
                      file=sys.stderr)
                rc = 1
            elif not schema_only:
                budget = int(data["budget-ns"])
                for key in ("max-ns", "p99-ns"):
                    if key in data and re.match(r"^\d+$", data[key]):
                        value = int(data[key])
                        if value > budget:
                            print(
                                f"{path}: line {start_line}: `{key}` {value} "
                                f"exceeds budget-ns {budget}",
                                file=sys.stderr)
                            rc = 1
    if not blocks:
        print(f"{path}: no rt-budget blocks yet; schema only", file=sys.stderr)
    perf_blocks = list(re.finditer(r"^```perf-evidence\n(.*?)^```",
                                   text, re.M | re.S))
    perf_ranges = []
    masked = text
    for block in perf_blocks:
        start = text[:block.start()].count("\n") + 1
        end = text[:block.end()].count("\n") + 1
        perf_ranges.append((start, end))
        masked = masked[:block.start()] + (" " * (block.end() - block.start())) + \
            masked[block.end():]
    for claim in PERF_CLAIM_RE.finditer(masked):
        line = text[:claim.start()].count("\n") + 1
        line_text = text.splitlines()[line - 1]

        if "Performance-oriented patches that claim" in line_text:
            continue
        if "must add a before/after evidence block" in line_text:
            continue
        if not any(abs(line - start) <= PERF_CLAIM_CONTEXT_LINES or
                   start <= line <= end
                   for start, end in perf_ranges):
            print(f"{path}: line {line}: performance claim needs nearby "
                  "`perf-evidence`", file=sys.stderr)
            rc = 1
    for block in perf_blocks:
        start_line = text[:block.start(1)].count("\n") + 1
        try:
            data = parse_block(block.group(1), start_line)
        except ValueError as exc:
            print(f"{path}: {exc}", file=sys.stderr)
            rc = 1
            continue
        for key in PERF_REQUIRED:
            if key not in data or not data[key]:
                print(f"{path}: line {start_line}: missing `{key}`", file=sys.stderr)
                rc = 1
        for key in ("baseline", "result"):
            if key in data and data[key] and not re.search(r"\d", data[key]):
                print(f"{path}: line {start_line}: `{key}` must name measured data",
                      file=sys.stderr)
                rc = 1
    if expect_text is not None:
        errs = sys.stderr.getvalue().splitlines()
        sys.stderr = real_stderr
        # schema-only notes are not errors
        errs = [e for e in errs if "schema only" not in e]
        if rc != 0 and (not errs or any(expect_text not in e for e in errs)):
            print(f"{path}: failed, but not only with '{expect_text}':",
                  file=sys.stderr)
            print("\n".join(errs), file=sys.stderr)
            return 1
    if expect_fail:
        if rc != 0:
            return 0
        print(f"{path}: negative fixture unexpectedly passed", file=sys.stderr)
        return 1
    return rc


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
