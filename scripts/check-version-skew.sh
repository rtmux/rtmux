#!/usr/bin/env bash
# Version-skew gate (TODO Q14): boot a kernel and an initramfs built from
# different trees and require roottask to report the mismatch exactly once.
#
#   scripts/check-version-skew.sh [--timeout SECONDS]
#   scripts/qemu-regress.sh --mode version-skew
#
# Two trees are built (OUT=build/skew-a and build/skew-b) with distinct
# RTMUX_BUILD_ID values.  The first boot takes the kernel from tree A and the
# initramfs from tree B (scripts/emu.sh EMU_KERNEL=) and must log exactly one
#   version skew: kernel=<id-a> roottask=<id-b>
# line.  The control boot uses tree B's own kernel and must log none.  Both
# boots must reach the login prompt.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOP_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$TOP_DIR" || exit 1

TIMEOUT=120
while [ $# -gt 0 ]; do
    case "$1" in
        --timeout) TIMEOUT="$2"; shift 2 ;;
        *) echo "check-version-skew: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

ID_A=skewaaaaaaaa
ID_B=skewbbbbbbbb
DIR_A="$TOP_DIR/build/skew-a"
DIR_B="$TOP_DIR/build/skew-b"
MAKE="${MAKE:-make}"
LOG_DIR="$(mktemp -d "${TMPDIR:-/tmp}/rtmux-version-skew.XXXXXX")" || exit 2
RUNNER=

cleanup()
{
    [ -n "$RUNNER" ] && kill "$RUNNER" 2>/dev/null
    rm -rf "$LOG_DIR"
    # The header is shared by every tree; put the normal build id back so the
    # next ordinary make does not see a stale one.
    "$MAKE" internal/include/rtmux-version.h >/dev/null 2>&1
}
trap cleanup EXIT

fail()
{
    echo "RTMux version-skew: FAIL ($*)"
    exit 1
}

build_tree()
{
    local out="$1" id="$2" goals="$3"

    rm -rf "$out"
    "$MAKE" OUT="$out" RTMUX_BUILD_ID="$id" $goals >/dev/null ||
        fail "build of $out failed"
    [ -f "$out/arch/riscv/rtmux-kernel" ] ||
        fail "$out is missing its kernel"
}

# boot LOG KERNEL: boot tree B's initramfs under KERNEL until the login prompt
# or the timeout; the run is stopped by PID once the prompt appears.
boot()
{
    local log="$1" kernel="$2" waited=0

    : >"$log"
    BUILD_DIR="$DIR_B" EMU_KERNEL="$kernel" CPUS=1 \
        timeout "$TIMEOUT" ./scripts/emu.sh </dev/null >"$log" 2>&1 &
    RUNNER=$!
    while kill -0 "$RUNNER" 2>/dev/null && [ "$waited" -lt "$TIMEOUT" ]; do
        grep -q '^login:' "$log" && break
        sleep 1
        waited=$((waited + 1))
    done
    # Let late roottask lines land before judging the log.
    sleep 2
    kill "$RUNNER" 2>/dev/null
    wait "$RUNNER" 2>/dev/null
    RUNNER=
    grep -q '^login:' "$log" || fail "no login prompt ($log)"
}

echo "RTMux version-skew: BEGIN"
# Only tree A's kernel is used; tree B supplies the initramfs too.
build_tree "$DIR_A" "$ID_A" startup
build_tree "$DIR_B" "$ID_B" "startup initramfs"
[ -f "$DIR_B/initramfs.cpio" ] || fail "$DIR_B is missing its initramfs"

# 1. Skew: kernel from A, initramfs from B.
boot "$LOG_DIR/skew.log" "$DIR_A/arch/riscv/rtmux-kernel"
n=$(grep -c 'version skew:' "$LOG_DIR/skew.log")
[ "$n" -eq 1 ] || fail "expected exactly one skew line, saw $n"
grep -q "version skew: kernel=$ID_A roottask=$ID_B" "$LOG_DIR/skew.log" ||
    fail "skew line does not name both ids"
grep 'version skew:' "$LOG_DIR/skew.log"

# 2. Control: kernel and initramfs from the same tree.
boot "$LOG_DIR/control.log" "$DIR_B/arch/riscv/rtmux-kernel"
n=$(grep -c 'version skew:' "$LOG_DIR/control.log")
[ "$n" -eq 0 ] || fail "matching kernel and roottask logged $n skew line(s)"

echo "RTMux version-skew: PASS"
