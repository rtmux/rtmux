#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# Q85 diagnostic: lock-free debug_thread() reader vs blocked_on publication.
#
# tests/spin/debug-blocked-on-reader.pml is a REPRODUCTION gate for an open
# issue, not a proof-of-correctness gate; the model does not prove that any
# kernel change is correct and no fix exists in the tree.  Each scenario must
# violate its own tagged assertion, and the replayed trail must show the
# expected reader observation (state, blocked_on):
#   PUBLISH, PUBSEND, COMPLETE, RECVPUB  debug_null_deref
#                 observes (REPLY|SEND|REPLY|RECEIVE, NULL)
#   RECV, SELF    debug_cross_type  observes (RECEIVE, thread) / (REPLY, self)
# PUBLISH is the local reply-block path (state written directly; NULL only if the
# previous blocked_on was NULL).  PUBSEND is the local send-block path (block()
# clears).  RECV's writer is remote COF_NETCON only (remote_ipc_msg), not local
# send.  All scenarios need CONFIG_PROCESSORS_MAX > 1.  JOIN and other writers
# are not modelled; see the model header.
# Controls (abstract, sequentially consistent; see the model header):
#   SERIAL_CTRL   reader pair and whole writer mutually exclusive: clean
#                 (exhaustive), and PROBE_PRE / PROBE_POST each must trip their
#                 own tagged assertion (probe_pre / probe_post), showing it can
#                 still observe both the old and the new state: clean != vacuous
#   WRITER_SPLIT  writer stores locked one by one: same tagged violation as
#                 baseline, and the replayed trail shows the same observed pair
#   GUARD_NULL    NULL tolerated: clean for NULL scenarios; the cross-type
#                 scenarios must STILL violate debug_cross_type, replayed with
#                 the same observed pair
# Invalid macro combinations must be rejected by the model (#error).
# RVWMO is not modelled, so "clean" is an SC result only.
#
# Needs Spin and a C compiler.  Writes only to a mktemp directory.
set -eu

command -v spin >/dev/null 2>&1 || { echo "debug-blocked-on: spin not found" >&2; exit 1; }
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "debug-blocked-on: '$CC' not found" >&2; exit 1; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MODEL=${DEBUG_BLOCKED_ON_MODEL:-$ROOT/tests/spin/debug-blocked-on-reader.pml}
[ -f "$MODEL" ] || { echo "debug-blocked-on: model '$MODEL' not found" >&2; exit 1; }
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

fail() { echo "debug-blocked-on: $*" >&2; exit 1; }

# run <name> <scenario> [-Dmacro...]: errors in $work/<name>.n, output in
# $work/<name>.out, trail in $work/<name>/.
run() {
    name=$1
    sc=$2
    shift 2
    mkdir "$work/$name"
    cp "$MODEL" "$work/$name/m.pml"
    (cd "$work/$name" && spin -a "-DSC_$sc" "$@" m.pml) >"$work/$name.spin" 2>&1 ||
        { cat "$work/$name.spin" >&2; fail "Spin failed for $name"; }
    (cd "$work/$name" && "$CC" -O2 -o pan pan.c) >"$work/$name.cc" 2>&1 ||
        { cat "$work/$name.cc" >&2; fail "compiler failed for $name"; }
    rc=0
    (cd "$work/$name" && ./pan -m10000) >"$work/$name.out" 2>&1 || rc=$?
    if [ "$rc" -ge 128 ]; then
        cat "$work/$name.out" >&2
        fail "pan killed by signal $((rc - 128)) for $name"
    fi
    [ "$rc" -eq 0 ] || { cat "$work/$name.out" >&2; fail "pan failed for $name (exit $rc)"; }
    sed -n 's/.*errors:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$work/$name.out" |
        head -n 1 >"$work/$name.n"
    [ -s "$work/$name.n" ] || fail "$name: no error count in pan output"
    if grep -Ei 'search depth too small|out of memory' "$work/$name.out" >/dev/null; then
        fail "incomplete search for $name"
    fi
    # A search that stops at its first (expected) violation is "not completed";
    # a clean run must be exhaustive.
    if [ "$(cat "$work/$name.n")" = 0 ] &&
        grep -i 'search not completed' "$work/$name.out" >/dev/null; then
        fail "incomplete search for $name"
    fi
}

errs() { cat "$work/$1.n"; }

# Spin pads its assertion text with a variable amount of whitespace, so match
# "assertion violated", then the exact tag in parentheses, tolerantly.
has_assert() { # <tag> <file>
    grep -Eq "assertion violated[[:space:]]+!\\([[:space:]]*$1[[:space:]]*\\)" "$2"
}

expect_violation() { # <name> <tag>
    [ "$(errs "$1")" != 0 ] || fail "$1: expected a violation, found none"
    has_assert "$2" "$work/$1.out" || fail "$1: violation is not the $2 assertion"
}

expect_clean() {
    [ "$(errs "$1")" = 0 ] || fail "$1: control violated (errors: '$(errs "$1")')"
}

expect_probe_fires() { # <name> <tag>: probe_pre / probe_post, not just any violation
    [ "$(errs "$1")" != 0 ] || fail "$1: probe did not fire (serial control would be vacuous)"
    has_assert "$2" "$work/$1.out" || fail "$1: violation is not the $2 probe assertion"
}

# expect_rejected <name> <scenario> <message> [-Dmacro...]: reject for that reason.
expect_rejected() {
    name=$1
    sc=$2
    expected=$3
    shift 3
    mkdir "$work/$name"
    cp "$MODEL" "$work/$name/m.pml"
    if (cd "$work/$name" && spin -a "-DSC_$sc" "$@" m.pml) >"$work/$name.spin" 2>&1; then
        fail "$name: incompatible macros were accepted"
    fi
    grep -Fq "$expected" "$work/$name.spin" || fail "$name: rejected for the wrong reason"
}

# replay <name> <tag> <state> <blocked_on>: the trail must reach the tagged
# assertion AND the reader must have observed exactly the expected pair.
replay() {
    rc=0
    (cd "$work/$1" && ./pan -r >"../$1.replay" 2>&1) || rc=$?
    if [ "$rc" -ge 128 ]; then
        cat "$work/$1.replay" >&2
        fail "$1: trail replay killed by signal $((rc - 128))"
    fi
    [ "$rc" -eq 0 ] || fail "$1: trail replay failed (exit $rc)"
    has_assert "$2" "$work/$1.replay" || fail "$1: trail replay did not reach $2"
    [ "$(grep -c '^OBS s=' "$work/$1.replay")" = 1 ] || fail "$1: expected one observation"
    grep -Eq "^OBS s=$3 b=$4([^0-9]|\$)" "$work/$1.replay" ||
        fail "$1: replayed reader did not observe state=$3 blocked_on=$4"
}

# <scenario> <tag> <state> <blocked_on> (ST_SEND=1 ST_REPLY=2 ST_RECEIVE=3; B_NULL=0 B_THR=3 B_SELF=4)
for row in "PUBLISH debug_null_deref 2 0" "PUBSEND debug_null_deref 1 0" "COMPLETE debug_null_deref 2 0" \
    "RECVPUB debug_null_deref 3 0" "RECV debug_cross_type 3 3" "SELF debug_cross_type 2 4"; do
    set -- $row
    sc=$1 tag=$2 os=$3 ob=$4
    lc=$(echo "$sc" | tr 'A-Z' 'a-z')

    run "$lc" "$sc"
    expect_violation "$lc" "$tag"
    replay "$lc" "$tag" "$os" "$ob"

    run "$lc-serial" "$sc" -DSERIAL_CTRL
    expect_clean "$lc-serial"
    run "$lc-pre" "$sc" -DSERIAL_CTRL -DPROBE_PRE
    expect_probe_fires "$lc-pre" probe_pre
    run "$lc-post" "$sc" -DSERIAL_CTRL -DPROBE_POST
    expect_probe_fires "$lc-post" probe_post

    run "$lc-split" "$sc" -DWRITER_SPLIT
    expect_violation "$lc-split" "$tag"
    replay "$lc-split" "$tag" "$os" "$ob"

    run "$lc-guard" "$sc" -DGUARD_NULL
    if [ "$tag" = debug_null_deref ]; then
        expect_clean "$lc-guard"
    else
        expect_violation "$lc-guard" "$tag"
        replay "$lc-guard" "$tag" "$os" "$ob"
    fi
done

expect_rejected bad-two-sc PUBLISH "define exactly one SC_* scenario" -DSC_RECV
expect_rejected bad-probe-alone PUBLISH "only meaningful with SERIAL_CTRL" -DPROBE_PRE
expect_rejected bad-probe-both PUBLISH "PROBE_PRE and PROBE_POST are mutually exclusive" -DSERIAL_CTRL -DPROBE_PRE -DPROBE_POST
expect_rejected bad-probe-guard PUBLISH "cannot be combined with GUARD_NULL" -DSERIAL_CTRL -DPROBE_PRE -DGUARD_NULL
expect_rejected bad-serial-split PUBLISH "SERIAL_CTRL and WRITER_SPLIT are mutually exclusive" -DSERIAL_CTRL -DWRITER_SPLIT
expect_rejected bad-guard-serial PUBLISH "GUARD_NULL is a separate control" -DSERIAL_CTRL -DGUARD_NULL

echo "debug-blocked-on: reproduced PUBLISH/PUBSEND/COMPLETE/RECVPUB (NULL) and RECV(remote only)/SELF (cross-type)," \
    "replayed with expected observations; SERIAL_CTRL clean and non-vacuous; WRITER_SPLIT" \
    "violates; GUARD_NULL removes NULL only; SC interleaving only, no kernel fix claimed"
