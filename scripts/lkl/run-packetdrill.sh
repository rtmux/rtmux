#!/bin/sh
# Optional out-of-tree packetdrill wrapper for LKL /net facade validation.
#
# No packetdrill cases are vendored here.  Point PACKETDRILL at the runner and
# LKL_PACKETDRILL_CASES at a directory of facade-compatible .pkt cases.
#
# SPDX-License-Identifier: Apache-2.0
set -eu

ROOT=$(CDPATH= cd "$(dirname "$0")/../.." && pwd -P)
OUT=${OUT:-"$ROOT/build/lkl-suite-reports"}
REPORT=${REPORT:-"$OUT/packetdrill-summary.txt"}
PACKETDRILL=${PACKETDRILL:-}
CASES=${LKL_PACKETDRILL_CASES:-}

mkdir -p "$OUT"

unsupported()
{
    {
        echo "suite packetdrill"
        echo "status UNSUPPORTED"
        echo "reason $*"
    } >"$REPORT"
    cat "$REPORT"
    exit 0
}

if [ -z "$PACKETDRILL" ]; then
    PACKETDRILL=$(command -v packetdrill || true)
fi
[ -n "$PACKETDRILL" ] || unsupported "packetdrill runner not found"
[ -n "$CASES" ] || unsupported "LKL_PACKETDRILL_CASES not set"
[ -d "$CASES" ] || unsupported "case directory not found: $CASES"

cases=$(find "$CASES" -type f -name '*.pkt' | sort)
[ -n "$cases" ] || unsupported "no facade-compatible .pkt cases in $CASES"

LOG="$OUT/packetdrill.log"
: >"$LOG"
: >"$REPORT.tmp"
pass=0
fail=0

for c in $cases; do
    if "$PACKETDRILL" "$c" >>"$LOG" 2>&1; then
        pass=$((pass + 1))
        printf 'PASS %s\n' "$c" >>"$REPORT.tmp"
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n' "$c" >>"$REPORT.tmp"
    fi
done

{
    echo "suite packetdrill"
    echo "log $LOG"
    echo "passed $pass"
    echo "failed $fail"
    if [ "$fail" -eq 0 ]; then
        echo "status PASS"
    else
        echo "status FAIL"
    fi
    cat "$REPORT.tmp"
} >"$REPORT"
rm -f "$REPORT.tmp"
cat "$REPORT"

[ "$fail" -eq 0 ]
