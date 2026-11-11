#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Frama-C verification gate (see formal/framac/README.md).
#
# Three stages, all deterministic pass/fail:
#
#   parse   every kernel source is accepted by Frama-C's front end, one file
#           at a time.  Catches syntax and typing regressions per translation
#           unit, which is all a per-file pass can see.
#   link    the same sources loaded as ONE program, with the linker's
#           conflicting-declaration warning escalated to an error.  This is
#           the stage that sees across translation units, so it is the one
#           that catches kernel/startup/lib/mem.c contradicting <string.h>.
#           The per-file stage cannot: mem.c parses clean in isolation
#           because it never includes the header it disagrees with.
#   wp      every ACSL contract written into a kernel source still
#           discharges, with RTE obligations included.  Contracts live in the
#           kernel sources themselves, so this proves the shipping body rather
#           than a copy of it.  A source opts in with a "framac-verify:"
#           marker naming the functions to prove.
#
# Coverage this gate does NOT have, stated here so a green run is not read as
# more than it is:
#   - kernel/ipc/fastmsg.c is excluded from every stage (Frama-C's parser
#     rejects a declaration directly after a label, which C23 allows).  That
#     is the largest IPC fast path, and it is unchecked.
#   - parse coverage is relative to the active .config: anything behind a
#     disabled CONFIG_* is never seen.
#   - the wp stage covers only the functions named in a framac-verify marker.
#     Today that is a handful in kernel/cap/table.c.  Everything else in the
#     kernel is unverified, and a green run says nothing about it.
#
# The arithmetic sweep (formal/framac/sweep-arith.sh) is deliberately not a
# gate: it runs far too long, and most of what it reports is missing
# contracts rather than defects.
#
# Needs Frama-C 33 or newer with WP and an SMT solver (opam switch frama-c-33,
# Alt-Ergo and Z3).
set -eu

cd "$(dirname "$0")/.."

JOBS=${JOBS:-6}
FC=formal/framac/fc.sh
# Frama-C 33's parser rejects a declaration directly after a label, which C23
# allows and gcc accepts (the "restart:" block in fastmsg.c).  Hoisting those
# declarations above the label would be behavior-preserving and would let this
# exclusion go.
PARSE_SKIP='kernel/ipc/fastmsg.c'
# A per-file source count far below this means the file list broke, not that
# the kernel shrank.  Without it an empty list reports "ok".
MIN_SOURCES=80

if ! command -v frama-c >/dev/null 2>&1; then
    echo "verify: frama-c not found (opam switch frama-c-33)" >&2
    exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

source_list() {
    find kernel -name '*.c' ! -path "$PARSE_SKIP" | sort >"$tmp/list"
    n=$(wc -l <"$tmp/list")
    if [ "$n" -lt "$MIN_SOURCES" ]; then
        echo "verify: only $n kernel sources found, expected at least" \
             "$MIN_SOURCES; wrong directory or a broken file list" >&2
        return 1
    fi
    echo "$n"
}

parse_stage() {
    n=$(source_list) || return 1
    echo "verify: parsing $n kernel sources individually (skipping $PARSE_SKIP)"
    xargs -P "$JOBS" -I@ sh -c "$FC @ >/dev/null 2>&1 || echo 'FAIL @'" \
        <"$tmp/list" >"$tmp/parse" 2>&1
    if [ -s "$tmp/parse" ]; then
        echo "verify: front end rejected these sources:" >&2
        cat "$tmp/parse" >&2
        return 1
    fi
    echo "verify: parse ok"
}

link_stage() {
    n=$(source_list) || return 1
    echo "verify: loading $n kernel sources as one program"
    # drop-conflicting-unused is a warning by default and Frama-C exits 0 on
    # it, so a cross-TU signature conflict passes unless it is escalated.
    # typing:incompatible-types-call is NOT escalated: kernel/core/sys.c's
    # MK_KSYSTABLE dispatch table casts deliberately, so it would fire on
    # correct code.  Those warnings are surfaced below instead.
    if ! xargs "$FC" -kernel-warn-key linker:drop-conflicting-unused=error \
            <"$tmp/list" >"$tmp/link" 2>&1; then
        echo "verify: whole-program load failed:" >&2
        grep -A3 -E 'User Error|Incompatible declaration' "$tmp/link" |
            head -20 >&2
        return 1
    fi
    if grep -q 'incompatible-types-call' "$tmp/link"; then
        echo "verify: note, incompatible function-pointer casts (not fatal," \
             "sys.c's dispatch table is deliberate):"
        grep -A2 'incompatible-types-call' "$tmp/link" |
            grep -oE 'kernel/[^:]*:[0-9]+' | sort -u | sed 's/^/  /'
    fi
    echo "verify: link ok"
}

# Read one framac-* marker out of a source.
#
# Deliberately not a line-anchored sed.  The tree runs clang-format ("make
# indent"), which reflows comments: a marker written as its own one-line
# comment gets merged into the neighbouring prose and wrapped across lines,
# and a pattern that expects "/* framac-verify: ... */" on one line then
# silently stops matching.  That happened.  Take everything between the key
# and the next framac-* key or the end of the comment, drop continuation
# asterisks, and collapse whitespace, so the value survives any reflow.
read_marker() {
    python3 -c '
import re, sys
text = open(sys.argv[1], encoding="utf-8", errors="replace").read()
m = re.search(re.escape(sys.argv[2]) + r":(.*?)(?=framac-[a-z-]+:|\*/)", text, re.S)
if m:
    print(" ".join(re.sub(r"^\s*\*", " ", m.group(1), flags=re.M).split()))
' "$1" "$2"
}

wp_stage() {
    # A source opts in with a "framac-verify:" marker naming the functions it
    # wants proved.  Function-scoped rather than file-scoped because a contract
    # creates obligations at every call site, so an annotated function whose
    # callers are still uncontracted would fail through no fault of its own.
    grep -rl 'framac-verify:' kernel --include='*.c' | sort >"$tmp/annotated"
    n=$(wc -l <"$tmp/annotated")
    if [ "$n" -eq 0 ]; then
        echo "verify: no kernel source opts in with a framac-verify marker" >&2
        return 1
    fi
    ok=0
    while read -r src; do
        fcts=$(read_marker "$src" framac-verify)
        mingoals=$(read_marker "$src" framac-min-goals)
        scan=$(read_marker "$src" framac-scan)
        assume=$(read_marker "$src" framac-assume)
        if [ -z "$fcts" ] || [ -z "$mingoals" ]; then
            echo "verify: $src needs both a framac-verify and a" \
                 "framac-min-goals marker" >&2
            ok=1
            continue
        fi
        set +e
        "$FC" "$src" -wp -wp-rte -wp-fct "$fcts" \
            -wp-cache none -wp-timeout 60 -wp-par "$JOBS" \
            -wp-prop='-@terminates' -wp-no-print >"$tmp/wp.log" 2>&1
        frc=$?
        set -e
        # The verdict is a separate program on purpose: as shell it needed
        # every $ doubled and could not be tested.  See its header for the
        # gates it applies and why "Proved goals: N / N" is not enough.
        python3 scripts/framac-verdict.py --log "$tmp/wp.log" --status "$frc" \
            --min-goals "$mingoals" --src "$src" --fcts "$fcts" \
            --assume "$assume" --scan $scan || ok=1
    done <"$tmp/annotated"
    return $ok
}

rc=0
case ${1:-all} in
parse) parse_stage || rc=1 ;;
link) link_stage || rc=1 ;;
wp) wp_stage || rc=1 ;;
all)
    parse_stage || rc=1
    link_stage || rc=1
    wp_stage || rc=1
    ;;
*)
    echo "usage: $0 [all|parse|link|wp]" >&2
    exit 2
    ;;
esac

[ "$rc" -eq 0 ] && echo "verify: ok"
exit $rc
