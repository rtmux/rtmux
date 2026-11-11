#!/usr/bin/env python3
#
# Emit build footprint rows and optional deltas against a previous report.
#
# SPDX-License-Identifier: Apache-2.0

import csv
import os
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
COLUMNS = ["kind", "path", "text", "data", "bss", "total", "bytes"]
NUMERIC = COLUMNS[2:]
REMOVED_SYSCALLS = {"__KSYS_MSG_READWRITEV"}


def rel(path):
    path = pathlib.Path(path)
    try:
        return path.resolve().relative_to(ROOT).as_posix()
    except ValueError:
        return path.as_posix()


def env_path(name):
    value = os.environ.get(name)
    return pathlib.Path(value) if value else None


def size_row(kind, path, strict_elf=False):
    size = os.environ.get("SIZE", "size")
    path = pathlib.Path(path)
    byte_count = path.stat().st_size
    try:
        out = subprocess.check_output([size, str(path)], text=True, stderr=subprocess.DEVNULL)
        fields = out.splitlines()[1].split()
        return {
            "kind": kind,
            "path": rel(path),
            "text": fields[0],
            "data": fields[1],
            "bss": fields[2],
            "total": fields[3],
            "bytes": str(byte_count),
        }
    except (IndexError, OSError, subprocess.CalledProcessError) as err:
        if strict_elf:
            raise SystemExit(f"footprint-report: {rel(path)} size parse failed") from err
        return {
            "kind": "blob",
            "path": rel(path),
            "text": "",
            "data": "",
            "bss": "",
            "total": "",
            "bytes": str(byte_count),
        }


def emit_csv(rows):
    writer = csv.DictWriter(sys.stdout, fieldnames=COLUMNS, lineterminator="\n")
    writer.writeheader()
    for row in rows:
        writer.writerow({name: row.get(name, "") for name in COLUMNS})


def add_if_exists(rows, kind, path, strict_elf=False):
    if path and pathlib.Path(path).is_file():
        rows.append(size_row(kind, path, strict_elf))


def initramfs_rows(rows, initramfs_dir):
    if not initramfs_dir or not pathlib.Path(initramfs_dir).is_dir():
        return
    root = pathlib.Path(initramfs_dir)
    for path in sorted(p for p in root.rglob("*") if p.is_file()):
        rows.append({
            "kind": "initramfs",
            "path": path.relative_to(root).as_posix(),
            "bytes": str(path.stat().st_size),
        })


def lkl_heap_rows(rows, out_dir):
    domain = ROOT / "servers/lkl/domain.h"
    if not domain.is_file() or not out_dir:
        return
    text = domain.read_text(encoding="utf-8")
    for suffix in ("fsd", "netd"):
        if not (out_dir / f"servers/lkl-{suffix}/lkl-{suffix}").is_file():
            continue
        macro = f"RTMUX_LKL_{suffix.upper()}_CMDLINE"
        match = re.search(rf"^#define {macro} .*mem=([0-9]+)M", text, re.M)
        if match:
            rows.append({
                "kind": "lkl-heap",
                "path": f"servers/lkl-{suffix}",
                "bytes": str(int(match.group(1)) * 1024 * 1024),
            })


def syscall_rows(rows):
    calls = ROOT / "internal/include/sys/calls.h"
    if not calls.is_file():
        return
    names = []
    for line in calls.read_text(encoding="utf-8").splitlines():
        match = re.match(r"\s*(__KSYS_[A-Z0-9_]+)(?:\s*=\s*([0-9]+))?", line)
        if not match:
            continue
        name = match.group(1)
        if name == "__KSYS_BAD" or "_SPARE" in name or name in REMOVED_SYSCALLS:
            continue
        names.append(name)
        rows.append({
            "kind": "surface",
            "path": f"syscall/{name}",
            "bytes": "1",
        })
    rows.append({"kind": "surface-total", "path": "syscall", "bytes": str(len(names))})


def priv_kcall_rows(rows):
    calls = ROOT / "kernel/include/private/priv-calls.h"
    if not calls.is_file():
        return
    text = calls.read_text(encoding="utf-8")
    match = re.search(r"#define RTMUX_PRIV_CALL_LIST\(X\)(.*?)(?:\n\n|enum \{)", text, re.S)
    if not match:
        return
    names = []
    for item in re.finditer(r"\bX\(([A-Z0-9_]+),\s*([A-Z0-9_]+),\s*([A-Za-z0-9_]+|NULL)\)", match.group(1)):
        name, cap, handler = item.groups()
        names.append(name)
        rows.append({
            "kind": "surface",
            "path": f"priv-kcall/{name}:{cap}:{handler}",
            "bytes": "1",
        })
    rows.append({"kind": "surface-total", "path": "priv-kcall", "bytes": str(len(names))})


def enforce_lkl_ceiling(rows):
    ceiling = 8 * 1024 * 1024
    for row in rows:
        if row["kind"] != "elf" or not row["path"].startswith("build/servers/lkl-"):
            continue
        total = int(row.get("total") or 0)
        if total > ceiling:
            print(
                f"footprint-report: {row['path']} total={total} exceeds LKL ceiling={ceiling}",
                file=sys.stderr,
            )
            return 1
    return 0


def load_report(path):
    lines = pathlib.Path(path).read_text(encoding="utf-8").splitlines()
    for i, line in enumerate(lines):
        if line == ",".join(COLUMNS):
            lines = lines[i:]
            break
    else:
        raise SystemExit(f"footprint-report: {path}: CSV header not found")
    return {
        (row["kind"], row["path"]): row
        for row in csv.DictReader(lines)
    }


def delta_rows(base_path, current):
    if not base_path:
        return []
    base = load_report(base_path)
    current_by_key = {(row["kind"], row["path"]): row for row in current}
    rows = []
    for key in sorted(set(base) | set(current_by_key)):
        before = base.get(key, {})
        after = current_by_key.get(key, {})
        delta = {"kind": "delta", "path": f"{key[0]}/{key[1]}"}
        changed = False
        for col in NUMERIC:
            old = int(before[col]) if before.get(col, "").lstrip("-").isdigit() else 0
            new = int(after[col]) if after.get(col, "").lstrip("-").isdigit() else 0
            diff = new - old
            if diff:
                changed = True
            delta[col] = str(diff)
        if changed:
            rows.append(delta)
    return rows


def main():
    out_dir = env_path("OUT")
    rows = []
    for name in ("KERNEL_BIN", "ROOTTASK_BIN", "LIBC_SO", "LDSO_SO"):
        add_if_exists(rows, "elf", env_path(name))
    if out_dir:
        for path in (
            out_dir / "servers/logger/logger",
            out_dir / "servers/pci/pci-server",
        ):
            add_if_exists(rows, "elf", path)
        for path in (
            out_dir / "servers/lkl-fsd/lkl-fsd",
            out_dir / "servers/lkl-netd/lkl-netd",
        ):
            add_if_exists(rows, "elf", path, strict_elf=True)
    add_if_exists(rows, "blob", env_path("INITRAMFS_CPIO"))
    initramfs_rows(rows, env_path("INITRAMFS_DIR"))
    lkl_heap_rows(rows, out_dir)
    syscall_rows(rows)
    priv_kcall_rows(rows)
    rows.extend(delta_rows(os.environ.get("FOOTPRINT_BASE"), rows))
    emit_csv(rows)
    return enforce_lkl_ceiling(rows)


if __name__ == "__main__":
    raise SystemExit(main())
