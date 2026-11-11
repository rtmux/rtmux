#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# IPC message-pool alloc/free pairing gate (section 13, C/litmus refinement
# rung, CBMC half).
#
# Model-checks the intrusive soul freelist that backs the IPC message/
# notification object pools (kernel/object/core.c) for sequential resource
# safety: no double hand-out, no double free, freelist integrity (no lost node,
# no cycle), and node conservation (no leak).  The harness mirrors the real
# pop/push pointer sequence; see tests/cbmc/ipc-pool-pairing.c.
#
# The gate also builds the harness with -DPLANT_BUG (which drops the freelist
# head advance on pop, handing out the same node twice) and requires that build
# to FAIL, so a green result cannot come from assertions that never fire.
#
# Needs CBMC (Debian/Ubuntu: apt-get install cbmc).
set -eu

. "$(dirname "$0")/gate-cert-lib.sh"
cert_begin

if ! command -v cbmc >/dev/null 2>&1; then
    echo "IPC CBMC: cbmc not found (apt-get install cbmc)" >&2
    exit 1
fi

UNWIND=12

# Each harness must verify clean, and each of its planted mutations must FAIL, so
# a green result can never come from assertions that never fire.  One mutation is
# listed per advertised property (a single mutation only proves the property it
# perturbs has teeth).
#   ipc-pool-pairing : soul freelist alloc/free pairing.  PLANT_BUG drops the
#     freelist head advance (double hand-out); PLANT_DOUBLE_FREE re-frees a node
#     (double free).
#   ipc-cap-authority: the cap check/use TOCTOU recheck.  PLANT_BUG drops the
#     recheck (an unauthorized reused-slot object is used).
failed_line() { # CBMC's "** N of M failed" summary, "no summary" if absent
    printf '%s\n' "$1" | sed -n 's/^\*\* \([0-9]* of [0-9]* failed\).*/\1/p' | head -n 1 |
        grep . || echo "no summary line"
}

check_harness() {
    harness=$1
    shift
    out=$(cbmc --unwind "$UNWIND" --unwinding-assertions "$harness" 2>&1)
    printf '%s\n' "$out"
    if ! printf '%s\n' "$out" | grep -q '^VERIFICATION SUCCESSFUL'; then
        echo "IPC CBMC: $harness did not verify" >&2
        exit 1
    fi
    cert_assert "$harness: VERIFICATION SUCCESSFUL, $(failed_line "$out") at --unwind $UNWIND with unwinding assertions"
    for mut in "$@"; do
        bug=$(cbmc --unwind "$UNWIND" "-D$mut" "$harness" 2>&1 || true)
        if ! printf '%s\n' "$bug" | grep -q '^VERIFICATION FAILED'; then
            echo "IPC CBMC: $harness with $mut did NOT fail; that assertion is vacuous" >&2
            exit 1
        fi
        cert_assert "$harness: -D$mut VERIFICATION FAILED as required, $(failed_line "$bug")"
    done
}

check_harness tests/cbmc/ipc-pool-pairing.c PLANT_BUG PLANT_DOUBLE_FREE
check_harness tests/cbmc/ipc-cap-authority.c PLANT_BUG

cert_result "both harnesses verify; every planted mutation FAILED"
echo "ipc-cbmc: ok"
