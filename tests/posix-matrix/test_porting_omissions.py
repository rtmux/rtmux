#!/usr/bin/env python3
"""Negative checks for validate_porting_omissions() in scripts/gen-posix-matrix.py.

The matrix and the omitted-interfaces table in docs/porting.md must agree, so
removing a table row, or adding a not-supported row with no table entry, must
each be reported.  Works on in-memory copies; no repository file is touched.
"""
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SCRIPTS = ROOT / "scripts"
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location("gen_posix_matrix",
                                              SCRIPTS / "gen-posix-matrix.py")
gen = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gen)

matrix = gen.OUT.read_text(encoding="utf-8")
porting = gen.PORTING_DOC.read_text(encoding="utf-8")
failures = []


def drop_row(text, name):
    return "\n".join(l for l in text.splitlines() if not l.startswith(f"| `{name}` |"))


def expect(label, errors, needle):
    if not any(needle in e for e in errors):
        failures.append(f"{label}: expected an error containing {needle!r}, got {errors}")


baseline = gen.validate_porting_omissions(matrix, porting)
if baseline:
    failures.append(f"baseline: unexpected errors {baseline}")

# A table row removed while its matrix row stays.
expect("removed table row",
       gen.validate_porting_omissions(matrix, drop_row(porting, "fork")),
       "fork: not-supported row has no omitted-interfaces table entry")

# A table row added with no matrix row.
extra = porting.rstrip("\n") + (
    "\n| `vfork` | `posix_spawn` | undefined reference at link time"
    " | `scripts/check-porting-contract.sh` |\n")
expect("added table row", gen.validate_porting_omissions(matrix, extra),
       "vfork: 0 not-supported matrix row(s), want 1")

# A not-supported matrix row removed while its table row stays.
expect("removed matrix row",
       gen.validate_porting_omissions(drop_row(matrix, "sbrk"), porting),
       "sbrk: 0 not-supported matrix row(s), want 1")

# A duplicated matrix row.
dup = matrix + "\n".join(l for l in matrix.splitlines() if l.startswith("| `link` |")) + "\n"
expect("duplicate matrix row", gen.validate_porting_omissions(dup, porting),
       "link: 2 not-supported matrix row(s), want 1")

for f in failures:
    print(f"test_porting_omissions: FAIL {f}", file=sys.stderr)
if failures:
    sys.exit(1)
print("test_porting_omissions: ok (4 negative cases)")
