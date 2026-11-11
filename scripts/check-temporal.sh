#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Temporal liveness/termination gate (section 13 abstract-model companion).
#
# Model-checks two temporal properties of the IPC/PI protocol with Spin:
#   ipc-rendezvous.pml  -- SRR rendezvous progress: a sender and receiver never
#                          both block forever with a partner available (the
#                          under-lock re-probe prevents the lost wakeup).
#   pi-walk-terminate.pml -- the PI boost walk terminates over any chain shape
#                          (unblocked owner => EOK, else EDEADLK).
#
# For each model the LTL property must hold (Spin reports errors: 0).  Each also
# has a committed non-vacuity witness: rebuilt with the guard removed
# (-DNO_REPROBE / -DNO_GUARD) the property MUST be violated (Spin finds an
# acceptance cycle), proving the property is falsifiable, not vacuously true.
#
# Needs Spin and a C compiler (known-good spin 6.5.2; apt-get install spin).
set -eu

. "$(dirname "$0")/gate-cert-lib.sh"
cert_begin

if ! command -v spin >/dev/null 2>&1; then
    echo "temporal: spin not found (apt-get install spin)" >&2
    exit 1
fi
CC=${CC:-cc}
if ! command -v "$CC" >/dev/null 2>&1; then
    echo "temporal: C compiler '$CC' not found" >&2
    exit 1
fi

ROOT=$(cd "$(dirname "$0")/.." && pwd)

verify() { # <pml> -> Spin verification output for the LTL claim
    work=$(mktemp -d)
    cp "$ROOT/$1" "$work/m.pml"
    shift
    (cd "$work" && spin -a "$@" m.pml >/dev/null &&
        "$CC" -O2 -o pan pan.c >/dev/null 2>&1 &&
        ./pan -a -m200000 2>&1)
    rm -rf "$work"
}

errors_of() { # Spin's "errors: N" count from a pan run, empty if absent
    printf '%s\n' "$1" | sed -n 's/.*errors: \([0-9]*\).*/\1/p' | head -n 1
}

check_model() { # <pml> <plant-macro>
    pml=$1
    plant=$2

    out=$(verify "$pml")
    held=$(errors_of "$out")
    if [ "$held" != 0 ]; then
        echo "temporal: $pml property does not hold" >&2
        exit 1
    fi
    cert_assert "$pml: LTL property holds (errors: $held)"
    out=$(verify "$pml" "-D$plant")
    broke=$(errors_of "$out")
    if [ -z "$broke" ] || [ "$broke" = 0 ]; then
        echo "temporal: $pml with -D$plant did not violate the property; the non-vacuity witness is vacuous" >&2
        exit 1
    fi
    cert_assert "$pml: -D$plant violates it (errors: $broke)"
}

check_model tests/spin/ipc-rendezvous.pml NO_REPROBE
check_model tests/spin/pi-walk-terminate.pml NO_GUARD

cert_result "both LTL properties hold; both non-vacuity witnesses violated"
echo "temporal: ok"
