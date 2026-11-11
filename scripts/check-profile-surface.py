#!/usr/bin/env python3
"""Preprocess <unistd.h> per profile and check the advertised surface end-to-end.

The other section-12.1 gates validate the profile *model*: the registry's
internal consistency (check-posix-matrix / validate_profiles), the trim header
matching the registry (check-posix-profile-header), and the config fragments
matching it (check-posix-profile-config).  None runs the real preprocessor, so a
bug that only surfaces when the compiler actually applies <unistd.h> plus the
generated trim header -- a `#undef` that does not take effect because the macro
is (re)defined by a later include, a mis-guarded trim block, a macro the static
regex parser cannot see -- passes every static gate yet ships the wrong surface.

This gate closes that gap by running the compiler and checking two things:

  1. Full agreement: the `_POSIX_*` macros the compiler advertises (value != -1)
     under the default FULL profile equal posix_profile.advertised_posix_options()
     -- the registry's static parser.  Catches the parser drifting from what the
     compiler actually sees.
  2. Trim delta: each smaller profile has exactly the options the registry says it
     drops (`trimmable - POSIX_PROFILES[profile]`, the very set the trim header
     `#undef`s) removed from the DEFINED macro set, and adds nothing.  Definedness
     matters, not just value: the trim contract is `#undef`, and RTMux headers/
     tests gate on `#if defined(_POSIX_X)`, so a dropped option reintroduced as
     `#define _POSIX_X -1` (defined-but-unsupported) must still fail here.

The compiler and its include flags come from the environment (`CC`, `PP_FLAGS`)
so the Makefile owns them; with no cross compiler on PATH the gate skips (rc 0),
like check-lkl-floor.

SPDX-License-Identifier: Apache-2.0
"""

import os
import pathlib
import re
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from posix_profile import (OPTION_REQUIREMENTS, POSIX_PROFILES,  # noqa: E402
                           advertised_posix_options)

# Each smaller profile's Kconfig -D; FULL is the default and defines none.
GUARD = {
    "minimal": "CONFIG_POSIX_PROFILE_MINIMAL",
    "realtime": "CONFIG_POSIX_PROFILE_REALTIME",
    "lkl": "CONFIG_POSIX_PROFILE_LKL",
}

_DEFINE = re.compile(r"^#define (_POSIX_[A-Z0-9_]+) (.+)$")


def macros(cc, flags, guard=None):
    """Preprocess <unistd.h> for a profile; return {`_POSIX_*` name: value}."""
    argv = cc + flags + ["-E", "-dM", "-x", "c", "-"]
    if guard:
        argv.append(f"-D{guard}=1")
    proc = subprocess.run(argv, input="#include <unistd.h>\n",
                          capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stderr.strip() or "preprocess failed")
    return {m.group(1): m.group(2).strip()
            for line in proc.stdout.splitlines() if (m := _DEFINE.match(line))}


def advertised(defs):
    """The advertised options: defined with a value other than -1 (unsupported)."""
    return {name for name, value in defs.items() if value != "-1"}


def main():
    cc = shlex.split(os.environ.get("CC", "riscv-none-elf-gcc"))
    flags = shlex.split(os.environ.get("PP_FLAGS", ""))
    if not cc or shutil.which(cc[0]) is None:
        print(f"profile-surface check: skipped (no {cc[0] if cc else 'CC'})")
        return 0

    errors = []
    try:
        full = macros(cc, flags)
    except RuntimeError as exc:
        print(f"profile-surface check failed:\n  full: preprocess failed: {exc}",
              file=sys.stderr)
        return 1
    full_defined = set(full)

    # 1. the compiler's FULL advertised surface must match the static parser.
    parsed = set(advertised_posix_options())
    advertised_full = advertised(full)
    only_cc, only_parser = sorted(advertised_full - parsed), sorted(parsed - advertised_full)
    if only_cc or only_parser:
        errors.append("full: compiler surface != advertised_posix_options()"
                      + (f"; compiler-only {only_cc}" if only_cc else "")
                      + (f"; parser-only {only_parser}" if only_parser else ""))

    # 2. each smaller profile has exactly trimmable - P #undef'd (gone from the
    #    DEFINED set), and defines nothing FULL does not.
    trimmable = frozenset(OPTION_REQUIREMENTS) & POSIX_PROFILES["full"]
    for name, guard in GUARD.items():
        try:
            got_defined = set(macros(cc, flags, guard))
        except RuntimeError as exc:
            errors.append(f"{name}: preprocess failed: {exc}")
            continue
        want_drop = trimmable - POSIX_PROFILES[name]
        gone = full_defined - got_defined
        unexpected = sorted(gone - want_drop)         # dropped but should not be
        not_dropped = sorted(want_drop - gone)        # should be #undef'd but is still defined
        added = sorted(got_defined - full_defined)
        if unexpected or not_dropped or added:
            errors.append(f"{name}: trim delta wrong"
                          + (f"; wrongly dropped {unexpected}" if unexpected else "")
                          + (f"; not dropped {not_dropped}" if not_dropped else "")
                          + (f"; wrongly added {added}" if added else ""))

    if errors:
        print("profile-surface check failed:", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print(f"profile-surface check: ok ({len(GUARD) + 1} profiles preprocessed)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
