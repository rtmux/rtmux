#!/bin/sh
# RTMux is freely redistributable under the Apache License. See the file
# "LICENSE" for information on usage and redistribution of this file.
#
# SPDX-License-Identifier: Apache-2.0
#
# IPC fence-pairing litmus gate (section 13, C/litmus refinement rung).
#
# Machine-checks the WAIT_PENDING bridge-bit handoff in kernel/ipc/fastmsg.c
# under the RISC-V weak memory model: the waiter publishes its coherent state
# then clears WAIT_PENDING behind a cpu_smp_mb(), and the waker spins on the bit
# then reads the state behind a cpu_smp_mb().  The forbidden lost-wakeup outcome
# (waker sees the bit cleared but reads stale state) must be unreachable.
#
# The paired control litmus removes the two message-passing ordering fences and
# must show the SAME outcome becomes reachable, so the gate also proves those
# fences are load-bearing rather than accidentally-passing.  (The waiter's
# post-clear store-buffer drain fence is a liveness concern, not part of this
# stale-state MP safety proof, so it is not modeled here.)  Run
# `eval $(opam env --switch=rtmux-herd)` first on the reference host (see AGENTS.md).
set -eu

if ! command -v herd7 >/dev/null 2>&1; then
    echo "IPC litmus: herd7 not found" >&2
    exit 1
fi

# Real pairing: the forbidden outcome must be Never.
out=$(herd7 tests/litmus/ipc-wait-pending-mp.litmus)
printf '%s\n' "$out"
if ! printf '%s\n' "$out" |
     grep -Eq '^Observation IPC-WAIT-PENDING-MP Never 0([[:space:]]|$)'; then
    echo "IPC litmus: WAIT_PENDING fence pairing allowed the lost-wakeup outcome" >&2
    exit 1
fi

# Control: with the fences removed the outcome must become reachable, proving
# the fences carry the ordering.  Require an EXPLICIT reachable observation
# (Sometimes/Always with a non-zero count); a missing Observation line (herd
# error, renamed test, output-format change) must fail rather than pass
# vacuously.
ctl=$(herd7 tests/litmus/ipc-wait-pending-mp-nofence.litmus)
printf '%s\n' "$ctl"
if ! printf '%s\n' "$ctl" |
     grep -Eq '^Observation IPC-WAIT-PENDING-MP-NOFENCE (Sometimes|Always) [1-9]'; then
    echo "IPC litmus: fence-removed control did not show the outcome reachable; the fences are not proven load-bearing (or herd produced no result), so the real litmus proves nothing" >&2
    exit 1
fi

# Narrowing proof: the ISOLATED WAIT_PENDING bridge-bit handshake stays race-free
# with the minimal message-passing fences (fence w,w publish, fence r,r observe).
# This licenses narrowing only the publish-side fence to w,w; the observe sites
# also order a prior MSG_DELIVERY claim store and stay full (see the barrier
# audit in docs/smp-contract.md).  Kept proven for the audit's "narrowable?"
# column; not applied in code (measure-or-revert, shared cpu_smp_mb macro).
weak=$(herd7 tests/litmus/ipc-wait-pending-mp-weak.litmus)
printf '%s\n' "$weak"
if ! printf '%s\n' "$weak" |
     grep -Eq '^Observation IPC-WAIT-PENDING-MP-WEAK Never 0([[:space:]]|$)'; then
    echo "IPC litmus: the w,w/r,r narrowing is no longer RVWMO-safe; update the barrier audit" >&2
    exit 1
fi

echo "ipc-litmus: ok"
