#!/bin/sh
set -eu

if ! command -v herd7 >/dev/null 2>&1; then
    echo "RCU litmus: herd7 not found" >&2
    exit 1
fi

out=$(herd7 tests/litmus/rcu-sched-qs-publish.litmus)
printf '%s\n' "$out"
if ! printf '%s\n' "$out" |
     grep -Eq '^Observation RCU-SCHED-QS-PUBLISH Never 0([[:space:]]|$)'; then
    echo "RCU litmus: QS publish ordering allowed the forbidden outcome" >&2
    exit 1
fi

echo "rcu-litmus: ok"
