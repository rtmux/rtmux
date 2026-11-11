#!/bin/sh
# LKL runtime probe.
#
# Build and boot the LKL Linux-domain servers under the `lkl` POSIX profile and
# run the LKL smoke modes, proving the lkl profile actually hosts them -- the
# runtime leg of "derive and enforce the LKL-capable profile floor" (the static
# enforcement is scripts/check-lkl-profile.py).  qemu-regress forces
# CONFIG_LKL_SERVERS=y, which the profile permits (Kconfig `depends on` +
# mk/toolchain.mk both allow lkl/full).
#
# qemu-regress builds build/lkl-regress incrementally; reusing it across a
# profile switch leaves it in an inconsistent link state (a stale libc.so with a
# hidden __swbuf breaks servers/logger), so this forces a clean build first.
#
# Standalone, NOT part of `make check`: it is a full LKL build plus a boot, and
# needs the externals/lkl-rv64/ artifacts and mkfs.ext4.  The full profile is
# restored on every exit path.
#
# Usage: scripts/probe-lkl-profile.sh [mode...]   (default: lkl-fsd-tree
# lkl-netd-http).
#
# SPDX-License-Identifier: Apache-2.0
set -eu

cd "$(dirname "$0")/.."
MAKE="${MAKE:-make}"
AUTOCONF=internal/include/generated/autoconf.h
if [ "$#" -eq 0 ]; then
    set -- lkl-fsd-tree lkl-netd-http
fi

# Restore FULL on any exit -- normal, `set -e` failure, or a signal routed here
# by the INT/TERM traps.  Only .config is switched (the LKL build lands in
# build/lkl-regress, not the main tree), so restoring the config is enough; a
# failed restore is loud and forces a nonzero exit.
restore_full() {
    status=$?
    trap - EXIT INT TERM  # clear first: `exit` below must not re-enter this trap
    echo "== restore: full profile =="
    rm -f "$AUTOCONF"
    if ! DEFCONFIG=configs/profile-full.config "$MAKE" defconfig >/dev/null; then
        echo "probe-lkl-profile: FAILED to restore the full profile;" \
             "run 'make defconfig'" >&2
        exit 1
    fi
    exit "$status"
}
trap restore_full EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

echo "== apply: lkl profile =="
DEFCONFIG=configs/profile-lkl.config "$MAKE" defconfig >/dev/null
if ! grep -q "^CONFIG_POSIX_PROFILE_LKL=y" .config; then
    echo "probe-lkl-profile: profile-lkl.config did not select the lkl profile" >&2
    exit 2
fi
rm -rf build/lkl-regress  # force a clean LKL build (see header)

for mode in "$@"; do
    # Per-mode boot budget, matching the Makefile LKL_QEMU_MODES table so a
    # slower host does not false-fail (netd's http/dns round-trips are the long
    # pole); qemu-regress's own LKL default is only 60s.
    case "$mode" in
        lkl-host-selftest) timeout=60 ;;
        lkl-netd-http) timeout=300 ;;
        *) timeout=180 ;;
    esac
    echo "== lkl smoke: $mode (build under lkl profile + boot, ${timeout}s) =="
    ./scripts/qemu-regress.sh --mode "$mode" --timeout "$timeout"
done

echo "probe-lkl-profile: all passed ($*)"
