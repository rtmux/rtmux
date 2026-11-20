#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# A thread's base priority (thread_t.real_priority) and the priority lent to it
# (msg_loan, pend_loan, pi_loan) are written only through the accessors in
# kernel/include/thread-prio.h; kernel/sync/wait.c alone refreshes pi_loan from
# the turnstiles the thread owns. Any other write lets a loan overwrite the
# base, the bug these fields exist to prevent (docs/pi-pc-design.md "Base and
# Effective Priority"). This gate fails on an assignment, increment or atomic
# store to one of the fields anywhere else in the kernel. A self-test feeds it
# planted writes first, so the pattern cannot silently stop matching.
set -eu

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

FIELDS='(real_priority|msg_loan|pend_loan|pi_loan)'
ASSIGN="(->|\\.)${FIELDS}[[:space:]]*(([-+*/%|&^]|<<|>>)?=[^=]|\\+\\+|--)"
# A mutating atomic whose first argument names a field: the GCC __atomic_*
# builtins, the C11 atomic_*_explicit calls and the kernel's own atomic_or,
# atomic_clr_64, atomic_add_value and the like. Loads and fences do not match.
ATOMIC="\\b(__)?atomic_(store|exchange|compare_exchange|fetch|add|sub|and|or|xor|clr|set|toggle)(_[a-z0-9]+)*\\([^,]*(->|\\.)${FIELDS}[[:space:]]*,"

# Print every write in the files given, as file:line:text.
scan() {
    grep -HnE "$ASSIGN|$ATOMIC" "$@" 2>/dev/null || true
}

# Writes the rule allows: the accessor header; pi_loan in wait.c; and the
# debug snapshot copy, whose target is a debug_thread_t, not a thread.
allowed() {
    grep -vE '^kernel/include/thread-prio\.h:' |
        grep -vE '^kernel/sync/wait\.c:[0-9]+:.*->pi_loan' |
        grep -vE '^kernel/debug/thread-debug\.c:[0-9]+:[[:space:]]*dtp->real_priority = ' ||
        true
}

# Self-test: every planted write must be reported, and the planted reads not.
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
mkdir -p "$tmp/kernel/ipc"
printf 'void f(thread_t *thp) { thp->real_priority = thp->priority; }\n' \
    >"$tmp/kernel/ipc/planted.c"
printf 'void g(thread_t *thp) { __atomic_store_n(&thp->msg_loan, 3, 0); }\n' \
    >>"$tmp/kernel/ipc/planted.c"
printf 'void h(thread_t *thp) { atomic_or_64(&thp->msg_loan, 1); }\n' \
    >>"$tmp/kernel/ipc/planted.c"
printf 'void i(thread_t *thp) { thp->real_priority <<= 1; }\n' \
    >>"$tmp/kernel/ipc/planted.c"
printf 'void j(thread_t *thp) { thp->pend_loan %%= 2; }\n' \
    >>"$tmp/kernel/ipc/planted.c"
printf 'int k(thread_t *thp) { return thp->pi_loan >= 2 || thp->pi_loan <= 1; }\n' \
    >>"$tmp/kernel/ipc/planted.c"
printf 'int l(thread_t *thp) { return __atomic_load_n(&thp->msg_loan, 0); }\n' \
    >>"$tmp/kernel/ipc/planted.c"
# Lines 1-5 are the writes and 6-7 the reads: compare the matched lines, not
# only their number, so a read matched by mistake cannot offset a missed write.
planted=$(cd "$tmp" && scan kernel/ipc/planted.c | allowed | cut -d: -f2 |
    tr '\n' ' ')
if [ "$planted" != "1 2 3 4 5 " ]; then
    echo "check-prio-writes: self-test matched lines [$planted], want [1 2 3 4 5 ]" >&2
    exit 1
fi

bad=$(find kernel arch -name '*.[ch]' | sort | while read -r f; do scan "$f"; done |
    allowed)
if [ -n "$bad" ]; then
    echo "check-prio-writes: write to a thread's base or loan priority outside" >&2
    echo "kernel/include/thread-prio.h (use its accessors, such as" >&2
    echo "thread_set_base_prio, thread_lend_msg or thread_set_pend_loan):" >&2
    printf '%s\n' "$bad" >&2
    exit 1
fi
echo "check-prio-writes: ok"
