#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Object-lifecycle teardown SMT gate (section 13 abstract-model companion).
#
# Runs the Z3 transition-system model of the process / connection / blocked-
# thread lifecycle (tests/smt/lifecycle.smt2): a dead process holds nothing, a
# zombie has no threads, nfds counts client attaches only and never underflows,
# connect_t.links equals its holders and never goes negative, blocked_on is set
# iff blocked and never names a freed connection, and an rcvid pairs with a
# blocked sender of the owning process.  It is contract capture for three fixed
# teardown hazards (9717d81c, 95b4205a, 3607e29a), not a live-race gate.
#
# Three layers keep a green result from being vacuous:
#   1. the model must give 13 sat (base, trace and witness queries) and 11 unsat
#      (initial state and every invariant);
#   2. tests/smt/lifecycle-planted.smt2 breaks one operation per invariant and
#      must go all sat, and its shared region must equal the model's;
#   3. each in-place mutation of the real model below must change the model's
#      verdicts, turning an unsat (invariant) result into sat.
# Needs Z3 (Debian/Ubuntu: apt-get install z3).
set -eu

. "$(dirname "$0")/gate-cert-lib.sh"
cert_begin

if ! command -v z3 >/dev/null 2>&1; then
    echo "lifecycle SMT: z3 not found (apt-get install z3)" >&2
    exit 1
fi

MODEL=tests/smt/lifecycle.smt2
PLANTED=tests/smt/lifecycle-planted.smt2
WANT_SAT=13
WANT_UNSAT=11
WANT_PLANTED=9

count() { printf '%s\n' "$1" | grep -c "^$2\$" || true; }
shared() { sed -n '/^;; BEGIN-SHARED$/,/^;; END-SHARED$/p' "$1"; }

if [ -z "$(shared "$MODEL")" ] || [ "$(shared "$MODEL")" != "$(shared "$PLANTED")" ]; then
    echo "lifecycle SMT: shared region of $PLANTED differs from $MODEL" >&2
    exit 1
fi

out=$(z3 "$MODEL")
printf '%s\n' "$out"
if [ "$(count "$out" unknown)" -ne 0 ] ||
   [ "$(count "$out" sat)" -ne "$WANT_SAT" ] ||
   [ "$(count "$out" unsat)" -ne "$WANT_UNSAT" ]; then
    echo "lifecycle SMT: model did not verify (expected $WANT_SAT sat +" \
         "$WANT_UNSAT unsat, no unknown)" >&2
    exit 1
fi
cert_assert "$MODEL: $(count "$out" sat) sat, $(count "$out" unsat) unsat, $(count "$out" unknown) unknown"

pout=$(z3 "$PLANTED")
if [ "$(count "$pout" sat)" -ne "$WANT_PLANTED" ] || [ "$(count "$pout" unsat)" -ne 0 ] ||
   [ "$(count "$pout" unknown)" -ne 0 ]; then
    echo "lifecycle SMT: planted-bug model did not go all-sat; an invariant check is vacuous" >&2
    printf '%s\n' "$pout"
    exit 1
fi
cert_assert "$PLANTED: $(count "$pout" sat) sat, $(count "$pout" unsat) unsat"

# In-place mutations of the real model: each edits one operation, and the
# clean unsat must then turn sat.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
nmut=0
mutate() { # name sed-expression
    sed "$2" "$MODEL" > "$tmp/m.smt2"
    if cmp -s "$MODEL" "$tmp/m.smt2"; then
        echo "lifecycle SMT: mutation '$1' did not change the model" >&2
        exit 1
    fi
    mout=$(z3 "$tmp/m.smt2")
    printf '%s\n' "$mout" > "$tmp/v"
    # An invariant must notice it: some baseline unsat must turn sat.
    if ! printf '%s\n' "$out" | paste - "$tmp/v" 2>/dev/null | grep -q '^unsat	sat$'; then
        echo "lifecycle SMT: mutation '$1' was not detected by any invariant" >&2
        exit 1
    fi
    cert_assert "mutation $1: verdicts changed ($(count "$mout" sat) sat, $(count "$mout" unsat) unsat)"
    nmut=$((nmut + 1))
}
mutate reap-skips-channel-sweep 's/(and (= (st p) 1) (= (nchan p) 0)/(and (= (st p) 1)/'
mutate server-slot-bumps-nfds \
    's/(= q (mkP 0 (nthr p) (nchan p) (ncap p) (nfds p) (ncli p) (+ (nsrv p) 1)))/(= q (mkP 0 (nthr p) (nchan p) (ncap p) (+ (nfds p) 1) (ncli p) (+ (nsrv p) 1)))/'
mutate detach-without-slot-guard 's/(and (< (st p) 2) (> (ncli p) 0)/(and (< (st p) 2)/'
mutate drop-without-holder 's/(and (= (alive c) 1) (> (cref c) 0)/(and (= (alive c) 1)/'
mutate unblock-keeps-blocked-on 's/(cref c) 0 0 0$/(cref c) 0 0 (bon c)/'
mutate recycle-while-blocked 's/^  (and (= (bst c) 0)$/  (and (>= (bst c) 0)/'

cert_result "model checks held, every planted bug was satisfiable, $nmut mutations detected"
echo "lifecycle-smt: ok"
