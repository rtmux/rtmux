; RTMux is freely redistributable under the Apache License. See the file
; "LICENSE" for information on usage and redistribution of this file.
;
; SPDX-License-Identifier: Apache-2.0
;
; Teeth control for srr-pi-safety.smt2.  Each block models a BROKEN version of
; one operation, so the matching invariant's negation becomes satisfiable.  The
; gate requires every block here to be sat, one per invariant.

(set-logic QF_BV)

(define-fun umax ((a (_ BitVec 8)) (b (_ BitVec 8))) (_ BitVec 8)
  (ite (bvuge a b) a b))

(define-fun umin ((a (_ BitVec 8)) (b (_ BitVec 8))) (_ BitVec 8)
  (ite (bvule a b) a b))

; Invariant 1 broken: deboost forgets to recompute, leaving the owner boosted
; after the last waiter has left.
(push 1)
(declare-const base (_ BitVec 8))
(declare-const eff  (_ BitVec 8))
(assert (bvugt eff base)) ; still boosted with no waiters -> priority leak
(check-sat) ; expect sat
(pop 1)

; Invariant 2 broken: boost uses min instead of max, dropping the owner below
; the waiter that is blocked on it.
(push 1)
(declare-const pit (_ BitVec 8))
(declare-const wp  (_ BitVec 8))
(declare-const target (_ BitVec 8))
(assert (= target (umin pit wp))) ; bug: min, not max
(assert (bvult target wp))        ; the owner ends up below the waiter
(check-sat) ; expect sat
(pop 1)

; Invariant 3 broken: the walk drops the visited-stamp / depth cap, so a cyclic
; wait chain does not return EDEADLK (it would spin forever).
(push 1)
(declare-const revisit Bool)
(declare-const result (_ BitVec 2))
(assert revisit)               ; a cycle exists
(assert (not (= result #b01))) ; but EDEADLK was not returned
(check-sat) ; expect sat
(pop 1)

; Invariant 4 broken: an unlocked ready-list unlink lets both harts unlink the
; same node and both believe they claimed the thread.
(push 1)
(declare-const win1 Bool) (declare-const win2 Bool)
(assert win1) (assert win2) ; both harts claim the same thread
(check-sat) ; expect sat
(pop 1)

; Invariant 5 broken: skip the tid / pin / stale-owner revalidation and act on
; the raw decoded owner word even after the slot was recycled to a new tid.
(push 1)
(declare-const tid_ok Bool)
(declare-const act Bool)
(assert (not tid_ok)) ; recycled to a different tid
(assert act)          ; but acted anyway (no revalidation)
(check-sat) ; expect sat
(pop 1)
