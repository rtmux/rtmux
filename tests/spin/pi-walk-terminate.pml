/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Section 13 abstract-model companion: temporal termination of the PI boost
 * walk.
 *
 * Models sync_pi_propagate (kernel/sync/wait.c:537): the walk follows
 * waiter -> blocked_on_turnstile -> owner -> blocked_on_turnstile, recording a
 * visited stamp and stopping at depth SYNC_PI_MAX_DEPTH or on a revisit,
 * returning EDEADLK.  Over any chain shape -- acyclic, over-deep, or cyclic --
 * the walk must terminate (reach an unblocked owner => EOK, else EDEADLK).
 *
 * NODES is the number of chain nodes and CAP the depth cap; NODES > CAP so an
 * over-deep ACYCLIC chain (which hits the cap) is distinct from a cycle (which
 * hits the revisit stamp).  The cap is checked before the revisit stamp, as in
 * wait.c:551 (depth == SYNC_PI_MAX_DEPTH) ahead of the visited loop at :556.
 *
 * The chain is built nondeterministically, so Spin explores every shape.  LTL
 * `terminate` (<> walk_done) must hold.  Built with -DNO_GUARD the visited-stamp
 * and depth cap are dropped, a cyclic chain spins forever, and termination is
 * violated -- the committed non-vacuity witness.
 */

#define NODES 5 /* chain nodes 0..NODES-1; NODES itself means "unblocked" */
#define CAP 4   /* depth cap (analogue of SYNC_PI_MAX_DEPTH); NODES > CAP */

byte blocked_on[NODES]; /* blocked_on[i] = turnstile node i's owner is blocked
                         * on, or NODES to mean the owner is not blocked */
bool walk_done = false;
byte result; /* 0 = EOK (reached an unblocked owner), 1 = EDEADLK */

active proctype walk() {
    byte i = 0;
    byte x = 0;
    /* Nondeterministically shape the wait chain: every configuration is a run. */
    do
    :: i < NODES -> select(x : 0 .. NODES); blocked_on[i] = x; i = i + 1
    :: i >= NODES -> break
    od;

    bool visited[NODES];
    byte v = 0;
    do :: v < NODES -> visited[v] = false; v = v + 1 :: v >= NODES -> break od;

    byte cur = 0; /* start the walk at node 0 */
    byte depth = 0;

    do
    :: blocked_on[cur] == NODES ->
        result = 0; walk_done = true; break /* unblocked owner: EOK */
    :: else ->
#ifndef NO_GUARD
        if
        :: depth == CAP -> /* checked first, as at wait.c:551 */
            result = 1; walk_done = true; break /* depth cap: EDEADLK */
        :: depth != CAP ->
            if
            :: visited[cur] ->
                result = 1; walk_done = true; break /* revisit: EDEADLK */
            :: else ->
                visited[cur] = true;
                depth = depth + 1;
                cur = blocked_on[cur]
            fi
        fi
#else
        cur = blocked_on[cur] /* no stamp/cap: a cycle spins forever */
#endif
    od
}

ltl terminate { <> walk_done }
