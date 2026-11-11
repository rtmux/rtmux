; RTMux is freely redistributable under the Apache License. See the file
; "LICENSE" for information on usage and redistribution of this file.
;
; SPDX-License-Identifier: Apache-2.0
;
; Section 13 abstract-model companion: SRR + bounded-PI safety, N harts.
;
; An SMT (Z3) model of the cross-hart safety invariants of the priority-
; inheritance turnstile protocol (kernel/sync/wait.c) and the SRR dispatch, at
; the spec level.  The payoff invariants are the ones a single-core model cannot
; state.  Each invariant has a reachability check (expect sat) and a negation
; check (expect unsat); srr-pi-safety-planted.smt2 breaks each and must go sat.
;
; Scope: a spec-consistency model of the priority-order / bounded-walk / claim /
; generation shape.  It does NOT model the concurrent turnstile linked-list, the
; scheduler run queues, or RCU epoch mechanics; those are covered by rt-check and
; CONFIG_LOCK_DEBUG probes (see the refinement table in docs/pi-pc-design.md).
; Priorities are 8-bit, higher value = more urgent (PI boosts the number up).
;
; Faithful to: sync_pi_propagate visited-stamp + depth cap (wait.c:551-563),
; boost target = max(pi_target, waiter->priority) (wait.c:577-579), deboost
; recompute-from-remaining (docs/pi-pc-design.md "On unlock..."), the atomic
; STATE_RUNNING claim (kernel/sched/dispatch.c), and the tthread generation
; revalidation against recycled (pid,tid) (docs/pi-pc-design.md "Owner identity").

(set-logic QF_BV)

(define-fun umax ((a (_ BitVec 8)) (b (_ BitVec 8))) (_ BitVec 8)
  (ite (bvuge a b) a b))

; ---------------------------------------------------------------------------
; Invariant 1: no priority leak.  A thread's effective priority is the max of
; its base and the priorities of waiters on the turnstiles it owns; once the
; last waiter leaves, the deboost recomputes it back to base.
; eff = has_waiter ? max(base, top_waiter) : base.
(push 1)
(declare-const base (_ BitVec 8))
(declare-const top  (_ BitVec 8)) ; highest waiter priority
(declare-const has_waiter Bool)
(declare-const eff (_ BitVec 8))
(assert (= eff (ite has_waiter (umax base top) base)))
(assert has_waiter) (assert (bvugt top base)) ; a boost is in effect
(check-sat) ; sanity: sat (eff can exceed base)
(pop 1)

(push 1)
(declare-const base (_ BitVec 8))
(declare-const top  (_ BitVec 8))
(declare-const has_waiter Bool)
(declare-const eff (_ BitVec 8))
(assert (= eff (ite has_waiter (umax base top) base)))
(assert (not has_waiter))       ; the last waiter has left
(assert (not (= eff base)))     ; negation: priority did not return to base
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 2: a PI boost never lowers the owner below the waiter that triggered
; it.  sync_pi_propagate sets target = max(pi_target, waiter->priority)
; (wait.c:577-579), so the boosted owner outranks or ties the waiter.
(push 1)
(declare-const pit (_ BitVec 8)) ; existing pi target for the owner
(declare-const wp  (_ BitVec 8)) ; requesting waiter priority
(declare-const target (_ BitVec 8))
(assert (= target (umax pit wp)))
(assert (bvugt wp pit)) ; the waiter is the new maximum
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const pit (_ BitVec 8))
(declare-const wp  (_ BitVec 8))
(declare-const target (_ BitVec 8))
(assert (= target (umax pit wp)))
(assert (bvult target wp)) ; negation: boosted target below the waiter
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 3: bounded PI-walk termination.  The walk records each turnstile in
; a visited stamp and stops on a revisit (wait.c:556-562), at depth 16
; (wait.c:551), or when the owner is the original waiter (wait.c:570).  So a
; cyclic or over-deep wait chain terminates with EDEADLK instead of spinning
; unbounded under the chain lock (unbounded propagation there is a realtime
; bug).  result: 0 = EOK (reached an unblocked owner), 1 = EDEADLK.
(push 1)
(declare-const revisit Bool) ; a turnstile repeated within the walk
(declare-const cap_hit Bool) ; depth reached 16 without resolving
(declare-const result (_ BitVec 2))
(assert (= result (ite (or revisit cap_hit) #b01 #b00)))
(assert revisit) ; a cyclic wait chain
(check-sat) ; sanity: sat (EDEADLK on a cycle is reachable)
(pop 1)

(push 1)
(declare-const revisit Bool)
(declare-const cap_hit Bool)
(declare-const result (_ BitVec 2))
(assert (= result (ite (or revisit cap_hit) #b01 #b00)))
(assert (or revisit cap_hit))  ; a cycle or an over-deep chain
(assert (not (= result #b01))) ; negation: it did NOT return EDEADLK
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 4: no thread runs on two harts.  A hart claims a runnable thread by
; unlinking its ready-list node under the per-dispatch lock
; (select_thread_default / dispatch_unlink_ready, kernel/sched/dispatch.c:206,
; 257; runcpu/STATE_RUNNING are published afterward at :284,:362, they are not
; the claim primitive).  The lock serializes the two harts, so the one that
; unlinks the node first wins and the other finds it already gone.  h1_first is
; the lock order; win1/win2 are the unlink outcomes.
(push 1)
(declare-const h1_first Bool)
(declare-const win1 Bool) (declare-const win2 Bool)
; the hart that holds the lock first unlinks the node; the second finds it gone
(assert (= win1 h1_first))
(assert (= win2 (not h1_first)))
(check-sat) ; sanity: sat (exactly one hart wins)
(pop 1)

(push 1)
(declare-const h1_first Bool)
(declare-const win1 Bool) (declare-const win2 Bool)
(assert (= win1 h1_first))
(assert (= win2 (not h1_first)))
(assert (and win1 win2)) ; negation: both harts unlink the same ready node
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 5: no transition acts on a stale receiver.  A turnstile owner is a
; raw (pid, tid) word; sync_turnstile_owner_pin resolves it through the process
; vector, checks the resolved thread's tid matches, and pins it
; (wait.c:336-339); a stale-owner guard additionally refuses to re-pin an
; already-cleared identical owner word (wait.c:421).  thread_t carries no
; generation field, so a recycled slot is caught by the tid mismatch, the pin
; failure, or the stale-owner guard -- not a generation compare.  A transition
; proceeds only when all of those pass: act = pid_ok & tid_ok & pin_ok & !stale.
(push 1)
(declare-const pid_ok Bool) (declare-const tid_ok Bool)
(declare-const pin_ok Bool) (declare-const stale Bool)
(declare-const act Bool)
(assert (= act (and pid_ok tid_ok pin_ok (not stale))))
(assert pid_ok) (assert tid_ok) (assert pin_ok) (assert (not stale))
(check-sat) ; sanity: sat (a valid owner is acted on)
(pop 1)

(push 1)
(declare-const pid_ok Bool) (declare-const tid_ok Bool)
(declare-const pin_ok Bool) (declare-const stale Bool)
(declare-const act Bool)
(assert (= act (and pid_ok tid_ok pin_ok (not stale))))
(assert (not tid_ok)) ; the slot was recycled to a different tid
(assert act)          ; negation: acted despite the tid mismatch
(check-sat) ; unsat
(pop 1)
