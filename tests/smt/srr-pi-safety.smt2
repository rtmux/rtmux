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
; Faithful to: prio_chain_walk visited-stamp + depth cap (kernel/sync/wait.c),
; boost target = max(pi_target, waiter->priority) (sync_pi_target_locked), deboost
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
; it.  prio_chain_walk sets target = max(pi_target, waiter->priority)
; (sync_pi_target_locked), so the boosted owner outranks or ties the waiter.
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
; a visited stamp and stops on a revisit (prio_chain_walk), at depth 16
; (SYNC_PI_MAX_DEPTH), or when the owner is the original waiter.  So a
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
; (sync_lookup_owner_pin); a stale-owner guard additionally refuses to re-pin an
; already-cleared identical owner word (sync_turnstile_record_owner).  thread_t carries no
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

; ---------------------------------------------------------------------------
; Invariant 6: base and loans are separate.  A thread's effective priority is
; max(base, pi_loan, msg_loan) (thread_effective_prio, kernel/include/
; thread-prio.h).  SchedSet and the sporadic drop/return write the base only
; (thread_set_base_prio), so after a base change from base to base2 every live
; loan still holds the thread up, and once both loans end it runs at base2.
(push 1)
(declare-const base (_ BitVec 8)) (declare-const base2 (_ BitVec 8))
(declare-const pil (_ BitVec 8)) (declare-const msl (_ BitVec 8))
(declare-const eff (_ BitVec 8)) (declare-const eff2 (_ BitVec 8))
(assert (= eff (umax base (umax pil msl))))
(assert (= eff2 (umax base2 (umax pil msl))))
(assert (bvult base2 base)) (assert (bvugt pil base)) ; lowered while boosted
(assert (= eff2 eff))
(check-sat) ; sanity: sat (SchedSet lowers the base, the boost holds)
(pop 1)

(push 1)
(declare-const base2 (_ BitVec 8))
(declare-const pil (_ BitVec 8)) (declare-const msl (_ BitVec 8))
(declare-const eff2 (_ BitVec 8))
(assert (= eff2 (umax base2 (umax pil msl))))
; negation: a loan is lost, or no loan is left and eff is not the base
(assert (or (bvult eff2 pil) (bvult eff2 msl)
            (and (= pil #x00) (= msl #x00) (not (= eff2 base2)))))
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 7: bounded REPLY propagation.  prio_chain_walk (kernel/sync/wait.c)
; lends a REPLY-blocked thread's priority to its server one hop at a time and
; stops after CONFIG_PRIO_REPLY_DEPTH hops (D, default 8), counting the stop in
; prio_reply_depth_overflow.  For a REPLY chain of length L the walk boosts
; min(L, D) servers, and it records an overflow exactly when L > D; it never
; does more than D hops of work for one change.
(push 1)
(declare-const L (_ BitVec 8)) (declare-const D (_ BitVec 8))
(declare-const boosted (_ BitVec 8)) (declare-const overflow Bool)
(assert (= D #x08))
(assert (= boosted (ite (bvugt L D) D L)))
(assert (= overflow (bvugt L D)))
(assert (bvugt L D))
(check-sat) ; sanity: sat (a chain longer than the bound overflows)
(pop 1)

(push 1)
(declare-const L (_ BitVec 8)) (declare-const D (_ BitVec 8))
(declare-const boosted (_ BitVec 8)) (declare-const overflow Bool)
(assert (bvuge D #x01)) (assert (bvule D #x40)) ; Kconfig range 1..64
(assert (= boosted (ite (bvugt L D) D L)))
(assert (= overflow (bvugt L D)))
; negation: more than D hops, a silent stop, or a short chain cut off
(assert (or (bvugt boosted D) (and (bvugt L D) (not overflow))
            (and (bvule L D) (not (= boosted L)))))
(check-sat) ; unsat
(pop 1)
