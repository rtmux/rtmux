#!/usr/bin/env python3
"""Reject old `make foo-check` target names."""

from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
OLD_TARGET = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_-]*-check$")
MAKE_IN_DOC = re.compile(r"\bmake\s+([A-Za-z0-9][A-Za-z0-9_-]*-check)\b")


def makefile_targets():
    text = (ROOT / "Makefile").read_text(encoding="utf-8")
    for lineno, line in enumerate(text.splitlines(), 1):
        if not line or line[0].isspace() or line.startswith("#"):
            continue
        if line.startswith(".PHONY:"):
            for target in line.split(":", 1)[1].split():
                if OLD_TARGET.match(target):
                    yield (
                        f"Makefile:{lineno}: phony target `{target}` should "
                        "be `check-*`"
                    )
            continue
        if (
            ":=" in line
            or "+=" in line
            or "?=" in line
            or "=" in line.split(":", 1)[0]
        ):
            continue
        if ":" not in line:
            continue
        for target in line.split(":", 1)[0].split():
            if "%" not in target and OLD_TARGET.match(target):
                yield f"Makefile:{lineno}: target `{target}` should be `check-*`"


def doc_references():
    paths = [
        ROOT / "README.md",
        *sorted((ROOT / "docs").glob("*.md")),
    ]
    for path in paths:
        text = path.read_text(encoding="utf-8")
        for lineno, line in enumerate(text.splitlines(), 1):
            for match in MAKE_IN_DOC.finditer(line):
                yield (
                    f"{path.relative_to(ROOT)}:{lineno}: "
                    f"`{match.group(0)}` should be `make check-*`"
                )


def main():
    errors = [*makefile_targets(), *doc_references()]
    if errors:
        print("check-make-target-names: old make target spelling found", file=sys.stderr)
        for error in errors:
            print(error, file=sys.stderr)
        return 1
    print("check-make-target-names: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
