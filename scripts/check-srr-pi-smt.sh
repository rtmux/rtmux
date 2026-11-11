#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# SRR + bounded-PI safety SMT gate (section 13 abstract-model companion).
#
# Runs the Z3 model of the cross-hart safety invariants of the priority-
# inheritance turnstile protocol (kernel/sync/wait.c) and the SRR dispatch
# (tests/smt/srr-pi-safety.smt2): no priority leak (deboost returns to base),
# a boost never lowers the owner below its waiter, the PI walk is bounded to 16
# (terminates), no thread runs on two harts, and no transition acts on a stale
# (recycled-generation) receiver.  Each invariant has a reachability check (sat)
# and a negation check (unsat); the planted companion breaks each and must go
# sat, so no invariant passes vacuously.  Needs Z3 (the reference host has 4.8.12).
set -eu

. "$(dirname "$0")/gate-cert-lib.sh"
cert_begin

if ! command -v z3 >/dev/null 2>&1; then
    echo "SRR/PI SMT: z3 not found (apt-get install z3)" >&2
    exit 1
fi

MODEL=tests/smt/srr-pi-safety.smt2
PLANTED=tests/smt/srr-pi-safety-planted.smt2

count() { printf '%s\n' "$1" | grep -c "^$2\$" || true; }

out=$(z3 "$MODEL")
printf '%s\n' "$out"
if [ "$(count "$out" unknown)" -ne 0 ] ||
   [ "$(count "$out" sat)" -ne 5 ] ||
   [ "$(count "$out" unsat)" -ne 5 ]; then
    echo "SRR/PI SMT: model did not verify (expected 5 sat + 5 unsat, no unknown)" >&2
    exit 1
fi
cert_assert "$MODEL: $(count "$out" sat) sat, $(count "$out" unsat) unsat, $(count "$out" unknown) unknown"

pout=$(z3 "$PLANTED")
if [ "$(count "$pout" sat)" -ne 5 ] || [ "$(count "$pout" unsat)" -ne 0 ]; then
    echo "SRR/PI SMT: planted-bug model did not go all-sat; an invariant check is vacuous" >&2
    printf '%s\n' "$pout"
    exit 1
fi

cert_assert "$PLANTED: $(count "$pout" sat) sat, $(count "$pout" unsat) unsat"
cert_result "model checks held and every planted bug was satisfiable"
echo "srr-pi-smt: ok"
