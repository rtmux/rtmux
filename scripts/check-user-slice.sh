#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Typed user-pointer boundary gate (section 13, syscall-boundary layer).
#
# Model-checks user_slice_check() from kernel/include/user-slice.h (the shipped
# predicate, via USER_SLICE_MODEL) with tests/cbmc/user-slice.c, then requires
# every planted mutation to FAIL: the header copied with the NULL, wraparound or
# overlength rule deleted, and a boundary check that always passes.  A green
# result therefore cannot come from assertions that never fire.
#
# The QEMU half (null, wraparound, kernel-range, unmapped and overlength
# buffers) is tests/userland/user-ptr-negative.c, run by
# scripts/qemu-regress.sh --mode syscall-testing.
#
# Needs CBMC (Debian/Ubuntu: apt-get install cbmc).
set -eu

. "$(dirname "$0")/gate-cert-lib.sh"
cert_begin

if ! command -v cbmc >/dev/null 2>&1; then
    echo "user-slice: cbmc not found (apt-get install cbmc)" >&2
    exit 1
fi

H=tests/cbmc/user-slice.c
INC=kernel/include
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

summary() {
    printf '%s\n' "$1" |
        sed -n 's/^\*\* \([0-9]* of [0-9]* failed\).*/\1/p' | head -n 1 |
        grep . || echo "no summary line"
}

out=$(cbmc --unwind 4 --unwinding-assertions -I "$INC" "$H" 2>&1)
printf '%s\n' "$out"
printf '%s\n' "$out" | grep -q '^VERIFICATION SUCCESSFUL' || {
    echo "user-slice: $H did not verify" >&2
    exit 1
}
cert_assert "$H: VERIFICATION SUCCESSFUL, $(summary "$out")"

fail_expected() { # name, extra cbmc args...
    name=$1
    shift
    bug=$(cbmc --unwind 4 "$@" "$H" 2>&1 || true)
    printf '%s\n' "$bug" | grep -q '^VERIFICATION FAILED' || {
        echo "user-slice: mutation $name did NOT fail; assertion is vacuous" >&2
        exit 1
    }
    cert_assert "$H: $name VERIFICATION FAILED as required, $(summary "$bug")"
}

mutate() { # name, sed expression
    mkdir -p "$TMP/$1"
    sed "$2" "$INC/user-slice.h" >"$TMP/$1/user-slice.h"
    if cmp -s "$INC/user-slice.h" "$TMP/$1/user-slice.h"; then
        echo "user-slice: mutation $1 did not change the header" >&2
        exit 1
    fi
    fail_expected "$1" -I "$TMP/$1"
}

mutate NO_NULL_CHECK 's/!base ||//'
mutate NO_WRAP_CHECK 's/base + len < base ||//'
mutate NO_MAX_CHECK 's/len > max/0/'
fail_expected PLANT_BOUNDARY -DPLANT_BOUNDARY -I "$INC"

cert_result "harness verifies; every planted mutation FAILED"
echo "user-slice: ok"
