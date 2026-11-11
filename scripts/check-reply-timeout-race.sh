#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Q76(a) reproduction check: reply-timeout activation vs concurrent force_ready.
#
# tests/spin/reply-timeout-arm.pml models timeout_start(thp) running on the
# receiver's hart after MSG_DELIVERY is dropped, against force_ready ->
# timeout_stop(thp) on another hart.  This is a REPRODUCTION gate, not a
# proof-of-correctness gate: the unmodified model MUST violate the safety
# assertion (an orphan timer / TIMEOUT_ACTIVE bit on a thread whose reply wait
# was completed).  Two controls must NOT violate it:
#   -DNO_FORCE    no concurrent force_ready (the violation needs the racer)
#   -DSERIAL_CTRL start and stop atomic w.r.t. each other (the violation is the
#                 unsynchronised check/arm window, not the model plumbing)
# SERIAL_CTRL is NOT a proposed fix: no such lock exists, and deferred
# delivery (timeout_deferred_fire) is not covered.  The fix is left OPEN.
#
# Needs Spin and a C compiler.  Writes only to a mktemp directory.
set -eu

command -v spin >/dev/null 2>&1 || { echo "reply-timeout-race: spin not found" >&2; exit 1; }
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "reply-timeout-race: '$CC' not found" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

run() { # <name> [-Dmacro] -> pan output in $work/<name>.out, trail kept in $work/<name>/
    name=$1
    shift
    mkdir "$work/$name"
    cp "$ROOT/tests/spin/reply-timeout-arm.pml" "$work/$name/m.pml"
    if ! (cd "$work/$name" && spin -a "$@" m.pml) >"$work/$name.spin" 2>&1; then
        cat "$work/$name.spin" >&2
        echo "reply-timeout-race: Spin failed for $name" >&2
        return 1
    fi
    if ! (cd "$work/$name" && "$CC" -O2 -o pan pan.c) >"$work/$name.cc" 2>&1; then
        cat "$work/$name.cc" >&2
        echo "reply-timeout-race: compiler failed for $name" >&2
        return 1
    fi
    if ! (cd "$work/$name" && ./pan -m10000) >"$work/$name.out" 2>&1; then
        cat "$work/$name.out" >&2
        echo "reply-timeout-race: pan failed for $name" >&2
        return 1
    fi
    # The base search stops early on its expected first assertion violation.
    if grep -Ei 'search depth too small|out of memory' "$work/$name.out" >/dev/null ||
        { [ "$name" != base ] && grep -i 'search not completed' "$work/$name.out" >/dev/null; }; then
        echo "reply-timeout-race: incomplete search for $name" >&2
        return 1
    fi
    sed -n 's/.*errors: \([0-9]*\).*/\1/p' "$work/$name.out" | head -n 1
}

base=$(run base) || exit 1
nof=$(run noforce -DNO_FORCE) || exit 1
ser=$(run serial -DSERIAL_CTRL) || exit 1

[ -n "$base" ] && [ "$base" != 0 ] ||
    { echo "reply-timeout-race: model did not reproduce the race (errors: '${base:-none}')" >&2; exit 1; }
grep -q 'assertion violated' "$work/base.out" ||
    { echo "reply-timeout-race: violation is not the safety assertion" >&2; exit 1; }
[ "$nof" = 0 ] || { echo "reply-timeout-race: NO_FORCE control violated (errors: '${nof:-none}')" >&2; exit 1; }
[ "$ser" = 0 ] || { echo "reply-timeout-race: SERIAL_CTRL control violated (errors: '${ser:-none}')" >&2; exit 1; }

# Replay the counterexample and verify that it reaches the assertion.
if ! (cd "$work/base" && ./pan -r >../replay.out 2>&1); then
    cat "$work/replay.out" >&2
    echo "reply-timeout-race: trail replay failed" >&2
    exit 1
fi
grep -q 'assertion violated' "$work/replay.out" ||
    { echo "reply-timeout-race: trail replay did not reach the violation" >&2; exit 1; }

states=$(sed -n 's/^ *\([0-9]*\) states, stored.*/\1/p' "$work/base.out" | head -n 1)
echo "reply-timeout-race: reproduced (errors: $base, $states states); NO_FORCE and SERIAL_CTRL controls hold; fix OPEN"
