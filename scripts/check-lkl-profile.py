#!/usr/bin/env python3
"""Enforce the LKL-capable profile floor.

Two checks keep "RTMux runs LKL" an enforced claim rather than an assumption:

  (A) Registry coverage: every advertised option group whose libc functions the
      LKL servers actually pull (posix_profile.LKL_FLOOR, derived by
      scripts/lkl-floor.py) is in the `lkl` profile.  POSIX_PROFILES["lkl"] is
      built from exactly that rule, so this catches a later manual edit that
      drops an option the LKL servers depend on -- the lkl profile would then
      advertise less than the servers need.

  (B) Active-.config lint: `CONFIG_LKL_SERVERS=y` must pair with the lkl or full
      profile.  Kconfig enforces this with `depends on`, but Kconfiglib silently
      HIDES a now-invisible symbol instead of erroring, and mk/toolchain.mk reads
      .config raw (`-include`), so a stale or hand-edited `CONFIG_LKL_SERVERS=y`
      under minimal/realtime would still reach the build and fail at link.  With
      no .config this check skips (rc 0).

SPDX-License-Identifier: Apache-2.0
"""

import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
CONFIG = ROOT / ".config"
sys.path.insert(0, str(ROOT / "scripts"))
from posix_profile import (LKL_FLOOR, OPTION_REQUIREMENTS,  # noqa: E402
                           POSIX_PROFILES, advertised_posix_options)


# Match a bool set to y as leniently as GNU make's `-include .config` does:
# tolerate whitespace around `=` (a hand-edited `CONFIG_X = y` still sets the
# make variable) and any trailing content, but not `=yes`/`=y2` (\b after y).
def _set(text, sym):
    return re.search(rf"^[ \t]*{sym}[ \t]*=[ \t]*y\b", text, re.MULTILINE) is not None


def registry_errors():
    """(A) Every advertised option whose functions the LKL servers pull is in
    `lkl`.  The `advertised` filter mirrors POSIX_PROFILES["lkl"]'s own
    construction (posix_profile): only advertised options can be in a profile, so
    a non-advertised option whose functions the servers happen to pull is not a
    gap -- those functions live in libc regardless of the advertised macro."""
    errors = []
    lkl = POSIX_PROFILES["lkl"]
    advertised = frozenset(advertised_posix_options())
    for opt, fns in OPTION_REQUIREMENTS.items():
        pulled = set(fns) & LKL_FLOOR
        if opt in advertised and pulled and opt not in lkl:
            errors.append(f"{opt}: LKL servers pull {sorted(pulled)} but the "
                          "advertised option is not in the lkl profile")
    return errors


def config_errors():
    """(B) CONFIG_LKL_SERVERS=y must not pair with an effectively-smaller profile.

    The effective profile is what the trim header selects: mk/toolchain.mk emits a
    -D per selected profile and `include/sys/posix-profile-trim.h` takes the first
    of minimal/realtime/lkl (else full), so if `minimal` or `realtime` is set the
    build is effectively that -- smaller than lkl -- regardless of any other
    profile line.  So the gate is simply: LKL_SERVERS on while minimal or realtime
    is selected.  (lkl-alone, full-alone, lkl+full, or no profile line all
    resolve to lkl/full, which cover the LKL floor.)  This mirrors the make-time
    guard in mk/toolchain.mk for a stale/hand-edited .config that bypasses
    Kconfig's `depends on`.  No .config => skip (rc 0).
    """
    if not CONFIG.exists():
        return None  # signal "skipped"
    text = CONFIG.read_text(encoding="utf-8")
    if not _set(text, "CONFIG_LKL_SERVERS"):
        return []
    smaller = [p.lower() for p in ("MINIMAL", "REALTIME")
               if _set(text, f"CONFIG_POSIX_PROFILE_{p}")]
    if not smaller:
        return []
    return [f"CONFIG_LKL_SERVERS=y requires the lkl or full POSIX profile, but "
            f"{' and '.join(smaller)} is selected (it wins the trim header's "
            "#if/#elif) -- the LKL servers need the lkl-floor libc surface.  "
            "Select POSIX_PROFILE_LKL/FULL or unset CONFIG_LKL_SERVERS."]


def main():
    errors = registry_errors()
    cfg = config_errors()
    errors.extend(cfg or [])
    if errors:
        print("lkl-profile check failed:", file=sys.stderr)
        for err in errors:
            print(f"  {err}", file=sys.stderr)
        return 1
    scope = "registry" if cfg is None else "registry + .config"
    print(f"lkl-profile check: ok ({scope})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
