#!/bin/sh
# Per-profile boot probe.
#
# For each smaller POSIX build profile: apply its config fragment, force a full
# rebuild, and boot the image under QEMU running the syscall_testing suite.  This
# is the runtime proof the static gates cannot give -- that a profile which trims
# option macros still builds a complete image, boots to `login:`, and its
# now-profile-aware conformance probe (tests/userland/posix-options.c) agrees
# with the trimmed headers at run time (sysconf(_SC_X) == the advertised
# _POSIX_X, and the trimmed options simply are not probed).
#
# Standalone, NOT part of `make check`: every profile is a full `make -Bj`
# rebuild plus a boot, far too heavy for the default gate.  The FULL profile is
# always restored on exit (including on failure or interrupt), so the working
# tree is left as it was found.
#
# Env: PROBE_TIMEOUT=<seconds> bounds each boot (default 300).
#
# Usage: scripts/probe-posix-profiles.sh [profile...]   (default: the three
# smaller profiles; `full` is the restore baseline and need not be probed).
#
# SPDX-License-Identifier: Apache-2.0
set -eu

cd "$(dirname "$0")/.."
MAKE="${MAKE:-make}"
if [ "$#" -eq 0 ]; then
    set -- minimal realtime lkl
fi

# Restore FULL on any exit -- normal, `set -e` failure, or a signal routed here
# by the INT/TERM traps below.  A failed restore is loud and forces a nonzero
# exit even if the probes passed, so a half-switched tree is never silent.
restore_full() {
    status=$?
    trap - EXIT INT TERM  # clear first: `exit` below must not re-enter this trap
    echo "== restore: full profile =="
    if ! { DEFCONFIG=configs/profile-full.config "$MAKE" defconfig >/dev/null &&
           "$MAKE" -Bj >/dev/null; }; then
        echo "probe-posix-profiles: FAILED to restore the full profile;" \
             "run 'make defconfig && make -Bj'" >&2
        exit 1
    fi
    exit "$status"
}
trap restore_full EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

for prof in "$@"; do
    frag="configs/profile-$prof.config"
    if [ ! -f "$frag" ]; then
        echo "probe-posix-profiles: no fragment $frag" >&2
        exit 2
    fi
    want="CONFIG_POSIX_PROFILE_$(echo "$prof" | tr '[:lower:]' '[:upper:]')"
    echo "== profile: $prof (build) =="
    DEFCONFIG="$frag" "$MAKE" defconfig >/dev/null
    if ! grep -q "^$want=y" .config; then
        echo "probe-posix-profiles: $frag did not select $want" >&2
        exit 2
    fi
    "$MAKE" -Bj >/dev/null
    echo "== profile: $prof (boot + syscall_testing) =="
    ./scripts/qemu-regress.sh --mode syscall-testing --timeout "${PROBE_TIMEOUT:-300}"
    echo "== profile: $prof PASS =="
done

echo "probe-posix-profiles: all passed ($*)"
