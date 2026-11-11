#!/usr/bin/env python3
#
# PSE51 profile gate for RTMux.
#
# PSE51 is the minimal realtime POSIX profile: single-process assumptions,
# threads, signals, clocks/timers, device I/O, and the required realtime/thread
# option macros.  This check is intentionally narrower than
# docs/posix-compliance.md, which tracks the whole exported POSIX-shaped surface.

import pathlib
import re
import sys

from posix_profile import (PSE51_REQUIRED_MACROS, PSE51_REQUIRED_SYMBOLS,
                           posix_header_files)


ROOT = pathlib.Path(__file__).resolve().parents[1]

REQUIRED_SYMBOLS = PSE51_REQUIRED_SYMBOLS
REQUIRED_MACROS = PSE51_REQUIRED_MACROS


def read(path):
    return (ROOT / path).read_text(encoding="utf-8", errors="ignore")


def exported_symbols():
    out = set()
    decl = re.compile(
        r"^\s*(?:extern\s+)?(?:[A-Za-z_][\w\s\*\(\)]*?)\b"
        r"([A-Za-z_]\w*)\s*\([^;{]*\)\s*;",
        re.MULTILINE,
    )
    for path in posix_header_files():
        text = path.read_text(encoding="utf-8", errors="ignore")
        out.update(m.group(1) for m in decl.finditer(text))
    return out


def defined_symbols():
    out = set()
    for base in ("lib/libc",):
        for path in (ROOT / base).rglob("*.[cS]"):
            text = path.read_text(encoding="utf-8", errors="ignore")
            for name in REQUIRED_SYMBOLS:
                patterns = (
                    rf"\b{name}\s*\([^;]*\)\s*\{{",
                    rf"\(\s*{name}\s*\)\s*\([^;]*\)\s*\{{",
                )
                if any(re.search(pattern, text, re.MULTILINE) for pattern in patterns):
                    out.add(name)
    return out


def stub_symbols():
    text = read("lib/libc/rtmux/posix-stubs.c")
    fn = re.compile(
        r"^\s*(?:[A-Za-z_][\w\s\*]*\s+)+([A-Za-z_]\w*)\s*"
        r"\([^;]*\)\s*\{",
        re.MULTILINE,
    )
    return {m.group(1) for m in fn.finditer(text)}


def macro_defs():
    text = read("include/unistd.h")
    defined = {}
    undefined = set()
    for line in text.splitlines():
        m = re.match(r"\s*#\s*define\s+(_POSIX_[A-Z0-9_]+)\b(.*)", line)
        if m:
            defined[m.group(1)] = m.group(2).strip()
            continue
        m = re.match(r"\s*#\s*undef\s+(_POSIX_[A-Z0-9_]+)\b", line)
        if m:
            undefined.add(m.group(1))
    return defined, undefined


def main():
    exported = exported_symbols()
    defined = defined_symbols()
    stubs = stub_symbols()
    macros, undefs = macro_defs()
    errors = []

    for name in sorted(REQUIRED_SYMBOLS):
        if name not in exported:
            errors.append(f"{name}: missing public declaration")
        if name not in defined:
            errors.append(f"{name}: missing libc definition")
        if name in stubs:
            errors.append(f"{name}: still implemented in posix-stubs.c")

    for name in sorted(REQUIRED_MACROS):
        if name in undefs or name not in macros:
            errors.append(f"{name}: required PSE51 option is not advertised")

    if errors:
        print("PSE51 check failed:", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print("PSE51 check: ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
