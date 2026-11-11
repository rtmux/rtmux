#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Capability rights/revocation algebra SMT gate (section 13 abstract-model
# companion).
#
# Runs the Z3 model of the kernel/cap/table.c invariants (tests/smt/
# cap-algebra.smt2): rights attenuation, same-table delegation shares the group,
# cross-table transfer mints a fresh group, revoke kills the whole group, and
# independent roots stay isolated.  Each invariant has a sanity check (the
# scenario is reachable -> sat) and a negation check (the invariant cannot be
# violated -> unsat), so an unsat result is meaningful and not vacuous.
#
# The paired planted file breaks one operation per invariant and must go sat, so
# a green result cannot come from checks that can never fail.  Needs Z3
# (Debian/Ubuntu: apt-get install z3; the reference host has 4.8.12).
set -eu

. "$(dirname "$0")/gate-cert-lib.sh"
cert_begin

if ! command -v z3 >/dev/null 2>&1; then
    echo "cap SMT: z3 not found (apt-get install z3)" >&2
    exit 1
fi

MODEL=tests/smt/cap-algebra.smt2
PLANTED=tests/smt/cap-algebra-planted.smt2

count() { printf '%s\n' "$1" | grep -c "^$2\$" || true; }

out=$(z3 "$MODEL")
printf '%s\n' "$out"
# Five invariants, each with a reachability (sat) check and a negation (unsat)
# check; any "unknown" or a flipped result fails the gate.
if [ "$(count "$out" unknown)" -ne 0 ] ||
   [ "$(count "$out" sat)" -ne 5 ] ||
   [ "$(count "$out" unsat)" -ne 5 ]; then
    echo "cap SMT: model did not verify (expected 5 sat + 5 unsat, no unknown)" >&2
    exit 1
fi
cert_assert "$MODEL: $(count "$out" sat) sat, $(count "$out" unsat) unsat, $(count "$out" unknown) unknown"

pout=$(z3 "$PLANTED")
# One planted-bug block per invariant; each must be satisfiable, proving the
# invariant checks have teeth.  An unsat here means the corresponding real check
# passes vacuously.
if [ "$(count "$pout" sat)" -ne 5 ] || [ "$(count "$pout" unsat)" -ne 0 ]; then
    echo "cap SMT: planted-bug model did not go all-sat; an invariant check is vacuous" >&2
    printf '%s\n' "$pout"
    exit 1
fi

cert_assert "$PLANTED: $(count "$pout" sat) sat, $(count "$pout" unsat) unsat"
cert_result "model checks held and every planted bug was satisfiable"
echo "cap-smt: ok"
