#!/usr/bin/env python3
"""Link a reference to each external function declared by RTMux's public headers."""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--libc", required=True)
    parser.add_argument("--ldso", required=True)
    args = parser.parse_args()
    headers = sorted(Path("include").rglob("*.h"))
    includes = "".join(f"#include <{p.relative_to('include')}>\n" for p in headers)
    flags = [args.cc, "-Iinclude", "-Iinternal/include", "-march=rv64g", "-mabi=lp64d",
             "-fno-builtin", "-fPIC"]
    with tempfile.TemporaryDirectory() as tmp:
        source = Path(tmp, "probe.c")
        decls = Path(tmp, "prototypes")
        source.write_text(includes)
        subprocess.run(flags + ["-aux-info", str(decls), "-fsyntax-only", str(source)],
                       check=True)
        names = set()
        for line in decls.read_text().splitlines():
            if not line.startswith("/* include/") or "*/ extern " not in line:
                continue
            # GCC prints a space before each parameter list. Skip a
            # parenthesized pointer declarator, as in void (*signal (...)) (...).
            prototype = line.split("*/ extern ", 1)[1]
            match = re.search(r"\b(\w+) \((?!\*)", prototype)
            if not match:
                raise ValueError(f"unrecognized function declarator: {line}")
            names.add(match[1])
        if not names:
            raise ValueError("compiler emitted no public prototypes")
        refs = "".join(f"__typeof__(&{n}) const ref_{n} = &{n};\n" for n in sorted(names))
        source.write_text(includes + refs)
        subprocess.run(flags + ["-fuse-ld=lld", "-nostdlib", "-shared", "-Wl,-z,defs",
                                "-Wl,-melf64lriscv", str(source), args.libc, args.ldso,
                                "-o", str(Path(tmp, "probe.so"))], check=True)
        print(f"public prototypes: linked {len(names)} external function references")


if __name__ == "__main__":
    main()
