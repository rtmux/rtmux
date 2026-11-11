#!/usr/bin/env python3
"""Derive the LKL-domain libc floor.

The `lkl` POSIX profile must provide exactly the libc
surface the lkl-fsd/lkl-netd servers pull in -- and that floor is *measured*,
not asserted.  Scan the built server objects together with the LKL archives:
the LKL library links before libc.so and drags in libc calls the server objects
alone never name, so a scan of the server objects only would undercount (see
mk/lkl-server.mk and servers/lkl-fsd/Makefile).

A symbol is in the floor when it is undefined across the LKL side (server
objects + liblkl-rtmux.a), not defined anywhere within that set, and defined by
libc.  The recorded snapshot lives in posix_profile.LKL_FLOOR so the profile
registry can use it without rebuilding LKL.

Usage:
  lkl-floor.py            print the derived floor
  lkl-floor.py --check    compare against posix_profile.LKL_FLOOR; exit 1 on
                          drift.  Both modes need the LKL artifacts built
                          (CONFIG_LKL_SERVERS) and the prebuilt library present;
                          without them the tool reports "unavailable" and
                          --check skips (exit 0) rather than failing.

SPDX-License-Identifier: Apache-2.0
"""

import glob
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[1]
NM = os.environ.get("NM", "riscv-none-elf-nm")
LIBC_OBJ_GLOB = "build/lib/libc/**/*.o"

# The exact objects and archive each server link rule pulls in -- mirror the
# OBJS/LKL_OUT_NAME in servers/lkl-{fsd,netd}/Makefile, NOT a glob (the build
# dirs can hold stale .o files from an earlier layout that are not linked).
SERVER_INPUTS = (
    (
        ROOT / "build/servers/lkl-fsd",
        ROOT / "prebuilt/lkl-rv64/out-fsd/liblkl-rtmux.a",
        ("main.o", "pathbuild.o", "host.o", "server.o", "connectpath.o",
         "virtio.o", "host-selftest.o"),
    ),
    (
        ROOT / "build/servers/lkl-netd",
        ROOT / "prebuilt/lkl-rv64/out-netd/liblkl-rtmux.a",
        ("main.o", "host.o", "server.o", "connectpath.o", "virtio.o"),
    ),
)


def _symbols(files):
    """Return (undefined, global-defined) symbol name sets across `files` via
    nm, or None if nm is unavailable.  Only globally-defined symbols
    (upper-case type, excluding debug `N`) satisfy a cross-object reference, so
    a file-local definition never masks a symbol that is really pulled from
    libc.  Undefined includes weak-undefined (`w`/`v`).  Calls are batched to
    stay under ARG_MAX."""
    undef, defd = set(), set()
    for i in range(0, len(files), 256):
        try:
            out = subprocess.run([NM, *files[i:i + 256]], check=True,
                                  capture_output=True, text=True).stdout
        except (FileNotFoundError, subprocess.CalledProcessError):
            return None
        for line in out.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] in ("U", "w", "v"):
                undef.add(parts[1])
            elif len(parts) == 3 and parts[1].isupper() and parts[1] != "N":
                defd.add(parts[2])
    return undef, defd


def _lkl_inputs():
    """The exact LKL-side inputs the link rules use, or None if any is missing
    (a partial tree must skip rather than derive a wrong floor)."""
    inputs = []
    for d, archive, names in SERVER_INPUTS:
        if not archive.exists():
            return None
        inputs.append(str(archive))
        for n in names:
            p = d / n
            if not p.exists():
                return None
            inputs.append(str(p))
    return inputs


def derive_floor():
    """Sorted list of libc symbols the LKL servers require, or None if the LKL
    artifacts, the LKL archives, the built libc, or nm are unavailable."""
    lkl_inputs = _lkl_inputs()
    libc_objs = glob.glob(str(ROOT / LIBC_OBJ_GLOB), recursive=True)
    if lkl_inputs is None or not libc_objs:
        return None
    lkl = _symbols(lkl_inputs)
    libc = _symbols(libc_objs)
    if lkl is None or libc is None:  # nm missing / failed
        return None
    undef, defd = lkl
    return sorted((undef - defd) & libc[1])


def main(argv):
    floor = derive_floor()
    check = "--check" in argv
    if floor is None:
        print("lkl-floor: LKL artifacts not built; nothing to derive.")
        return 0
    if check:
        sys.path.insert(0, str(ROOT / "scripts"))
        from posix_profile import LKL_FLOOR

        recorded = sorted(LKL_FLOOR)
        if floor != recorded:
            added = sorted(set(floor) - set(recorded))
            removed = sorted(set(recorded) - set(floor))
            print("lkl-floor: drift vs posix_profile.LKL_FLOOR", file=sys.stderr)
            if added:
                print("  added:   " + " ".join(added), file=sys.stderr)
            if removed:
                print("  removed: " + " ".join(removed), file=sys.stderr)
            print("  update posix_profile.LKL_FLOOR (run lkl-floor.py to list)",
                  file=sys.stderr)
            return 1
        print("lkl-floor: ok (%d symbols)" % len(floor))
        return 0
    print("# LKL libc floor: %d symbols" % len(floor))
    for s in floor:
        print(s)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
