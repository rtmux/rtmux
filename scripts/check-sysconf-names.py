#!/usr/bin/env python3
#
# Gate sysconf()'s list of valid names: every _SC_* that include/confname.h
# defines must appear as a case in sysconf_name_valid() in
# lib/libc/posix/sysconf.c, and the list must name nothing the header does not
# define. An alias (a name defined as another _SC_ name) is covered by its
# target. Without this, a new name would fail sysconf() with EINVAL.
#
# SPDX-License-Identifier: Apache-2.0

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "include/confname.h"
SOURCE = ROOT / "lib/libc/posix/sysconf.c"


def strip_comments(text):
    """Drop C comments, keeping their newlines, so a commented-out name counts
    as absent."""
    return re.sub(r"/\*.*?\*/|//[^\n]*",
                  lambda m: "\n" * m.group(0).count("\n"), text, flags=re.S)


def main():
    header = set()
    for name, value in re.findall(r"^#define\s+(_SC_\w+)[ \t]+([^\n]*)",
                                  strip_comments(HEADER.read_text()), re.M):
        value = value.strip()
        if re.fullmatch(r"_SC_\w+", value):
            continue  # an alias is covered by its target
        if not re.fullmatch(r"(0[xX][0-9a-fA-F]+|\d+)[uUlL]*", value):
            print(f"check-sysconf-names: cannot classify '#define {name} {value}'",
                  file=sys.stderr)
            return 1
        header.add(name)
    text = strip_comments(SOURCE.read_text())
    func = re.search(r"sysconf_name_valid\(int name\)\s*\{(.*?)\n\}", text, re.S)
    if not func:
        print("check-sysconf-names: sysconf_name_valid() not found", file=sys.stderr)
        return 1
    listed = set(re.findall(r"case\s+(_SC_\w+)\s*:", func.group(1)))
    missing, extra = sorted(header - listed), sorted(listed - header)
    for name in missing:
        print(f"check-sysconf-names: {name} is in confname.h but not in sysconf_name_valid()",
              file=sys.stderr)
    for name in extra:
        print(f"check-sysconf-names: {name} is in sysconf_name_valid() but not in confname.h",
              file=sys.stderr)
    if missing or extra:
        return 1
    print(f"check-sysconf-names: ok ({len(header)} names)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
