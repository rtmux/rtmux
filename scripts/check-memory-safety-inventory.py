#!/usr/bin/env python3
"""Validate the memory-safety inventory and its fail-closed fixture."""

import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQUIRED = (
    "ELF image loading",
    "initramfs CPIO parsing",
    "romfs block decoding",
    "user-copy and xfer vectors",
    "IPC message marshalling",
    "path handling",
    "DMA and MMIO mapping",
    "service restart and stale handles",
)


def make_targets() -> set[str]:
    text = (ROOT / "Makefile").read_text()
    return set(re.findall(r"^([A-Za-z0-9_.-]+)\s*:", text, re.MULTILINE))


def command_is_live(command: str, targets: set[str]) -> bool:
    command = command.strip("` ")
    words = command.split()
    if len(words) >= 2 and words[0] == "make":
        return words[1] in targets
    if words:
        return (ROOT / words[0]).exists()
    return False


def table_rows(path: Path) -> list[tuple[int, list[str]]]:
    rows = []
    for line_no, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line.startswith("|") or line.startswith("| ---"):
            continue
        cells = [cell.strip() for cell in line.strip("|").split("|")]
        if cells and cells[0] == "boundary":
            continue
        rows.append((line_no, cells))
    return rows


def check(path: Path) -> list[str]:
    errors = []
    targets = make_targets()
    rows = table_rows(path)
    seen = set()

    for line_no, cells in rows:
        if len(cells) != 6:
            errors.append(f"{path}: line {line_no}: expected 6 columns")
            continue
        boundary, trusted, maximum, lifetime, cleanup, command = cells
        seen.add(boundary)
        for name, value in (
            ("trusted input", trusted),
            ("maximum length", maximum),
            ("ownership or lifetime rule", lifetime),
            ("cleanup path", cleanup),
            ("test_command", command),
        ):
            if not value or value.upper() in {"TBD", "TODO", "N/A"}:
                errors.append(f"{path}: line {line_no}: empty {name}")
        if command and not command_is_live(command, targets):
            errors.append(f"{path}: line {line_no}: not a live command: {command}")

    for boundary in REQUIRED:
        if boundary not in seen:
            errors.append(f"{path}: missing boundary row: {boundary}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("path", nargs="?",
                        default="docs/memory-safety-inventory.md")
    parser.add_argument("--expect-fail", action="store_true")
    parser.add_argument("--expect-error")
    args = parser.parse_args()

    errors = check(ROOT / args.path)
    if args.expect_fail:
        if errors and (args.expect_error is None or
                       any(args.expect_error in error for error in errors)):
            print("check-memory-safety-inventory: negative fixture rejected")
            return 0
        if errors:
            print(f"{args.path}: failed for the wrong reason")
            for error in errors:
                print(error)
            return 1
        print(f"{args.path}: negative fixture unexpectedly passed")
        return 1
    if errors:
        for error in errors:
            print(error)
        return 1
    print("check-memory-safety-inventory: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
