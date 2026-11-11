#!/bin/sh
# Whole-kernel arithmetic sweep: WP over RTE's division, shift, and
# signed/unsigned overflow obligations, with memory-validity obligations
# turned off.
#
# Why only arithmetic: a mem_access obligation asks whether a pointer is
# valid, and nothing in this tree states that a cap table or a thread pointer
# points anywhere in particular, so every one of them comes back unproved and
# says only "this function has no contract".  Overflow obligations usually
# depend on local arithmetic alone, so an unproved one is a question worth
# asking.
#
# -wp-prop='@assert' restricts the run to RTE's own assertions.  Without it the
# sweep also reports contract obligations, so adding an ACSL contract anywhere
# makes every call site of it show up here as an unproved "arithmetic" goal.
#
# Output: formal/framac/sweep/<file>.log per source, and a summary on stdout.
set -e
TOP=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$TOP/formal/framac/sweep
JOBS=${JOBS:-6}
mkdir -p "$OUT"

if [ "$1" = --one ]; then
    src=$2
    rel=${src#"$TOP"/}
    log=$OUT/$(echo "$rel" | tr / -).log
    timeout 900 "$TOP/formal/framac/fc.sh" "$src" \
        -wp -wp-rte -rte-no-mem -wp-prop='@assert' \
        -wp-timeout 5 -wp-par 2 -wp-no-print >"$log" 2>&1 || true
    res=$(grep -E '^\[wp\] Proved goals:' "$log" | tail -1 | sed 's/.*Proved goals: *//')
    # Distinguish the two ways a file yields no ratio.  They are opposites: one
    # means there was no arithmetic to check, the other means the checking did
    # not finish, and reporting both as "none" hides every timeout among the
    # files that were simply quiet.
    if [ -z "$res" ]; then
        if grep -q 'No goal generated' "$log"; then res='-'; else res='TIMEOUT'; fi
    fi
    printf '%-16s %s\n' "$res" "$rel"
    exit 0
fi

[ $# -gt 0 ] || set -- $(find "$TOP/kernel" -name '*.c' ! -name fastmsg.c | sort)
for src; do echo "$src"; done |
    xargs -P "$JOBS" -n1 "$TOP/formal/framac/sweep-arith.sh" --one
