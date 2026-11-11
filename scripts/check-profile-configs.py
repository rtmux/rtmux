#!/usr/bin/env python3
"""Check the configs/profile-*.config fragments against the profile registry.

Each build-selectable POSIX profile has a defconfig-style
fragment under configs/ that selects it: `DEFCONFIG=configs/profile-<name>.config
make defconfig`.  This gate keeps three things in lockstep so a fragment cannot
silently mis-apply:

  registry (scripts/posix_profile.POSIX_PROFILES)
      <-> Kconfig choice members (the `config POSIX_PROFILE_*` entries)
      <-> the configs/profile-<name>.config fragments

For every registry profile there must be a Kconfig choice member
`POSIX_PROFILE_<UPPER>` and exactly one fragment whose ONLY assignment is
`CONFIG_POSIX_PROFILE_<UPPER>=y`.  A fragment that names an undefined symbol,
carries an unrelated assignment (`CONFIG_FOO=y`, `# CONFIG_BAR is not set`), or
double-selects would either be dropped by Kconfiglib (reverting to the default
FULL) or change unrelated build state -- both are rejected here.  (A profile that
later needs an extra symbol -- e.g. an LKL profile pairing with
`CONFIG_LKL_SERVERS` -- extends this allowlist deliberately.)

Pure text, no toolchain, so it runs in the same `make check` pass as the other
posix gates.

SPDX-License-Identifier: Apache-2.0
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIGS = ROOT / "configs"
sys.path.insert(0, str(ROOT / "scripts"))
from posix_profile import POSIX_PROFILES  # noqa: E402

# `# CONFIG_X is not set` is a real (negative) assignment in Kconfig .config
# files, not a comment -- match it before treating '#' lines as comments.
_UNSET = re.compile(r"^\s*#\s*(CONFIG_[A-Z0-9_]+)\s+is not set\s*$")
# A positive assignment; tolerate an inline comment, which Kconfiglib strips.
_ASSIGN = re.compile(r"^\s*(CONFIG_[A-Z0-9_]+)\s*=\s*(\S+?)\s*(?:#.*)?$")
# The Kconfig choice members driving the profile selection.
_MEMBER = re.compile(r"^\s*config\s+(POSIX_PROFILE_[A-Z]+)\s*$", re.MULTILINE)


def assignments(path):
    """Every Kconfig assignment in a fragment: {symbol: value} ('n' for unset).

    Plain comments and blank lines are ignored; a `# CONFIG_X is not set` line
    counts as `X: 'n'` because Kconfiglib applies it.
    """
    out = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        m = _UNSET.match(line)
        if m:
            out[m.group(1)] = "n"
            continue
        m = _ASSIGN.match(line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def kconfig_members():
    return {f"CONFIG_{m}"
            for m in _MEMBER.findall((ROOT / "Kconfig").read_text(encoding="utf-8"))}


def main():
    errors = []
    want = {name: f"CONFIG_POSIX_PROFILE_{name.upper()}" for name in POSIX_PROFILES}

    # registry <-> Kconfig: the choice must define exactly the registry profiles.
    members = kconfig_members()
    if members != set(want.values()):
        errors.append(
            f"Kconfig POSIX_PROFILE choice {sorted(members)} does not match the "
            f"registry {sorted(want.values())} -- a profile lacks its Kconfig "
            "choice member (Kconfiglib would drop the fragment and keep FULL) or "
            "vice versa")

    # registry <-> fragments: one fragment per profile, selecting only its member.
    for name, sym in want.items():
        path = CONFIGS / f"profile-{name}.config"
        if not path.exists():
            errors.append(f"{path.relative_to(ROOT)}: missing fragment for "
                          f"profile '{name}'")
            continue
        got = assignments(path)
        if got != {sym: "y"}:
            errors.append(f"{path.relative_to(ROOT)}: assignments {got} -- a "
                          f"profile fragment must set exactly {sym}=y and nothing "
                          "else")

    # No stray configs/profile-*.config beyond the registry profiles.
    for path in sorted(CONFIGS.glob("profile-*.config")):
        stray = path.stem[len("profile-"):]
        if stray not in POSIX_PROFILES:
            errors.append(f"{path.relative_to(ROOT)}: no profile '{stray}' in "
                          "POSIX_PROFILES")

    if errors:
        print("profile-config check failed:", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    print(f"profile-config check: ok ({len(want)} profiles)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
