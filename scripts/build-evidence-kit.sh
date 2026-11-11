#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)

python3 - "$ROOT" "$@" <<'PY'
import os
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

root = Path(sys.argv[1])
args = sys.argv[2:]
draft = False
if args == ["--draft"]:
    draft = True
elif args:
    raise SystemExit("usage: scripts/build-evidence-kit.sh [--draft]")

final_out = root / "build/evidence"
certs_dir = root / "build/certs"
out = root / "build/evidence.tmp"

required_docs = {
    "assumptions": root / "docs/safety-runtime.md",
    "limitations": root / "docs/safety-runtime.md",
    "safety concept": root / "docs/safety-runtime.md",
    "security concept": root / "docs/capability-law.md",
    "verification report": root / "docs/formal-claims.md",
    "trace matrix": root / "docs/safety-runtime.md",
    "residual risks": root / "docs/safety-runtime.md",
    "realtime budgets": root / "docs/rt-budgets.md",
    "fault recovery": root / "docs/fault-recovery.md",
}

full_checks = [
    ("make-check", ["make", "check"]),
    ("make-check-formal", ["make", "check-formal"]),
    ("make-check-rt", ["make", "check-rt"]),
]
cheap_checks = [
    ("check-evidence-claims", ["scripts/check-evidence-claims.sh"]),
    ("check-rt-budgets", ["python3", "scripts/rt-budgets-check.py", "docs/rt-budgets.md"]),
    ("check-fault-recovery-doc", ["scripts/check-fault-recovery.sh"]),
]


def run(argv, capture=False):
    try:
        return subprocess.run(
            argv,
            cwd=root,
            check=False,
            text=True,
            stdout=subprocess.PIPE if capture else None,
            stderr=subprocess.STDOUT if capture else None,
        )
    except OSError as err:
        if not capture:
            raise
        return subprocess.CompletedProcess(argv, 127, stdout=str(err) + "\n")


def section(text, name):
    match = re.search(rf"^## {re.escape(name)}\s*$", text, re.M)
    if not match:
        raise SystemExit(f"build-evidence-kit: missing section `{name}`")
    rest = text[match.end():]
    next_match = re.search(r"^## ", rest, re.M)
    end = next_match.start() if next_match else len(rest)
    return rest[:end].strip()


def table_rows(path):
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("|"):
            rows.append(line)
    return rows


def table_after(text, name):
    body = section(text, name)
    rows = [line for line in body.splitlines() if line.startswith("|")]
    if not rows:
        raise SystemExit(f"build-evidence-kit: missing table in `{name}`")
    return "\n".join(rows)


def write(name, text):
    path = out / name
    path.write_text(text.rstrip() + "\n", encoding="utf-8")
    return path


def tree_status():
    # Gate certificates live under the ignored build/certs/, so they never show here.
    return run(["git", "status", "--porcelain", "--", "."], capture=True).stdout.strip()


for path in (final_out, out):
    if path.exists():
        shutil.rmtree(path)

missing = [path for path in required_docs.values() if not path.is_file()]
if missing:
    for path in missing:
        print(f"build-evidence-kit: missing {path.relative_to(root)}", file=sys.stderr)
    raise SystemExit(1)

dirty = tree_status()
if dirty and not draft:
    print(dirty, file=sys.stderr)
    raise SystemExit("build-evidence-kit: working tree is dirty; rerun with --draft for preview")

check_lines = []
if not draft:
    # Every certificate in the kit must come from this run, not a leftover.
    shutil.rmtree(certs_dir, ignore_errors=True)
for name, argv in full_checks:
    if draft:
        check_lines.append(f"| {name} | `{' '.join(argv)}` | DRAFT-NOT-RUN |")
        continue
    rc = run(argv, capture=True)
    status = "PASS" if rc.returncode == 0 else "FAIL"
    check_lines.append(f"| {name} | `{' '.join(argv)}` | {status} |")
    if rc.returncode:
        print(rc.stdout or "", end="", file=sys.stderr)
        raise SystemExit(f"build-evidence-kit: {name} failed")

if not draft:
    rc = run(["python3", "scripts/gate_cert.py", "verify"], capture=True)
    status = "PASS" if rc.returncode == 0 else "FAIL"
    check_lines.append(
        f"| gate-certificates | `python3 scripts/gate_cert.py verify` | {status} |")
    if rc.returncode:
        print(rc.stdout or "", end="", file=sys.stderr)
        raise SystemExit("build-evidence-kit: gate certificates do not match HEAD")

for name, argv in cheap_checks:
    rc = run(argv, capture=True)
    status = "PASS" if rc.returncode == 0 else "FAIL"
    check_lines.append(f"| {name} | `{' '.join(argv)}` | {status} |")
    if rc.returncode:
        print(rc.stdout or "", end="", file=sys.stderr)
        raise SystemExit(f"build-evidence-kit: {name} failed")

footprint = run(["make", "-s", "footprint-report"], capture=True)
if footprint.returncode:
    print(footprint.stdout or "", end="", file=sys.stderr)
    raise SystemExit("build-evidence-kit: footprint-report failed")
dirty = tree_status()
if dirty and not draft:
    print(dirty, file=sys.stderr)
    raise SystemExit("build-evidence-kit: gate sequence dirtied the tree")
out.mkdir(parents=True, exist_ok=True)
write("footprint.csv", footprint.stdout)
if not draft:
    shutil.copytree(certs_dir, out / "certs", ignore=shutil.ignore_patterns("*.assert"))
check_lines.append("| footprint-report | `make -s footprint-report` | PASS |")

safety = (root / "docs/safety-runtime.md").read_text(encoding="utf-8")
formal = root / "docs/formal-claims.md"
rt_budgets = root / "docs/rt-budgets.md"
cap_law = root / "docs/capability-law.md"
fault = root / "docs/fault-recovery.md"
commit = run(["git", "rev-parse", "--short=12", "HEAD"], capture=True).stdout.strip()
clean = "yes" if tree_status() == "" else "no"
now = datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
source_label = f"commit `{commit}`" if clean == "yes" else f"dirty working tree based on commit `{commit}`"
cross = os.environ.get("CROSS_COMPILE", "riscv-none-elf-")
cc = os.environ.get("CC", cross + "gcc")
lld = shutil.which("ld.lld") or shutil.which("lld")
qemu = shutil.which("qemu-system-riscv64")
opensbi = os.environ.get("BIOS")
if not opensbi:
    bundled = Path("/usr/lib/riscv64-linux-gnu/opensbi/generic/fw_dynamic.bin")
    opensbi = str(bundled) if bundled.is_file() else "QEMU bundled default (-bios default)"


def version(argv):
    rc = run(argv, capture=True)
    if rc.returncode:
        return "missing"
    return (rc.stdout or "").splitlines()[0]


def opensbi_version(qemu_path, bios):
    if not qemu_path:
        return "missing"
    bios_arg = bios if bios != "QEMU bundled default (-bios default)" else "default"
    try:
        rc = subprocess.run(
            [
                qemu_path,
                "-machine",
                "virt",
                "-nographic",
                "-bios",
                bios_arg,
                "-smp",
                "1",
                "-m",
                "128M",
            ],
            cwd=root,
            check=False,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=3,
        )
        text = rc.stdout or ""
    except subprocess.TimeoutExpired as err:
        text = err.stdout or ""
        if isinstance(text, bytes):
            text = text.decode(errors="replace")
    match = re.search(r"^OpenSBI\s+v[0-9][^\s]*", text, re.M)
    if not match:
        return "missing"
    return f"{match.group(0)} ({bios})"


opensbi = opensbi_version(qemu, opensbi)
if opensbi == "missing" and not draft:
    raise SystemExit("build-evidence-kit: unable to determine OpenSBI version")

write("assumptions-of-use.md", "# Assumptions Of Use\n\n" + section(safety, "Assumptions Of Use"))
write("limitations.md", "# Limitations\n\n" + section(safety, "Limitations"))
write(
    "safety-concept.md",
    "# Safety Concept\n\n"
    + section(safety, "TCB Boundary")
    + "\n\n## Hazards\n\n"
    + section(safety, "Hazards")
    + "\n\n## Recovery\n\n"
    + section(fault.read_text(encoding="utf-8"), "Recovery Rules")
    + "\n\n## Fault Taxonomy\n\n"
    + table_after(fault.read_text(encoding="utf-8"), "Taxonomy"),
)
write(
    "security-concept.md",
    "# Security Concept\n\n"
    + "\n".join(table_rows(cap_law))
    + "\n\nSee `docs/capability-law.md` and `docs/capability-abi.md`.",
)
write(
    "trace-matrix.md",
    "# Trace Matrix\n\n"
    + section(safety, "Trace Matrix")
    + "\n\n## Formal Claims\n\n"
    + "\n".join(table_rows(formal)),
)
write("residual-risks.md", "# Residual Risks\n\n" + section(safety, "Residual Risks"))
write(
    "verification-report.md",
    "# Verification Report\n\n"
    "| check | command | status |\n| --- | --- | --- |\n"
    + "\n".join(check_lines)
    + "\n\n## Status Guide\n\n"
    + "- `Proven`, `Tested`, `Asserted`, and `Aspirational` are formal-claim statuses from "
    "`docs/formal-claims.md`.\n"
    + "- `measured` realtime rows carry current budget evidence; `experimental` rows are not "
    "release claims.\n"
    + "- Fault-recovery rows marked `tested` have executable evidence; `specified` rows are "
    "design contracts.\n"
    + "- Unsupported or deferred behavior stays in residual risks, limitations, or "
    "experimental rows.\n"
    + "\n\n## Claim Rows\n\n"
    + "\n".join(table_rows(formal))
    + "\n\n## Realtime Evidence Index\n\n"
    + table_after(rt_budgets.read_text(encoding="utf-8"), "Capability Realtime Evidence"),
)
write(
    "tool-versions.txt",
    "\n".join([
        f"generated: {now}",
        f"git-commit: {commit}",
        f"working-tree-clean: {clean}",
        f"python: {platform.python_version()}",
        f"platform: {platform.platform()}",
        f"make: {run(['make', '--version'], capture=True).stdout.splitlines()[0]}",
        f"git: {run(['git', '--version'], capture=True).stdout.strip()}",
        f"riscv-cross-gcc: {version([cc, '--version'])}",
        f"lld: {version([lld, '--version']) if lld else 'missing'}",
        f"qemu-system-riscv64: {version([qemu, '--version']) if qemu else 'missing'}",
        f"opensbi: {opensbi}",
        f"z3: {run(['sh', '-c', 'command -v z3 >/dev/null && z3 --version || true'], capture=True).stdout.strip() or 'missing'}",
        f"cbmc: {run(['sh', '-c', 'command -v cbmc >/dev/null && cbmc --version | head -n 1 || true'], capture=True).stdout.strip() or 'missing'}",
        f"spin: {run(['sh', '-c', 'command -v spin >/dev/null && spin -V || true'], capture=True).stdout.strip() or 'missing'}",
    ]),
)
write(
    "commands.md",
    "# Commands\n\n"
    "Run before consuming this kit:\n\n"
    "```sh\n"
    "scripts/build-evidence-kit.sh\n"
    "```\n\n"
    "Default mode requires clean source inputs, runs the full gate sequence, and records pass/fail "
    "status in `verification-report.md`. Use `--draft` only to preview output from a dirty tree; "
    "draft reports are not release evidence.",
)
write(
    "README.md",
    "# RTMux Evidence Kit\n\n"
    f"Generated from {source_label} at {now}.\n\n"
    "Files:\n\n"
    "- `assumptions-of-use.md`\n"
    "- `limitations.md`\n"
    "- `safety-concept.md`\n"
    "- `security-concept.md`\n"
    "- `verification-report.md`\n"
    "- `trace-matrix.md`\n"
    "- `tool-versions.txt`\n"
    "- `commands.md`\n"
    "- `residual-risks.md`\n"
    "- `footprint.csv`\n"
    "- `certs/<gate>.json` (per-gate provenance certificates; absent from a `--draft` kit)\n",
)

out.replace(final_out)
print(f"build-evidence-kit: wrote {final_out.relative_to(root)}")
PY
