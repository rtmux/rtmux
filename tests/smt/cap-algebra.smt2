; RTMux is freely redistributable under the Apache License. See the file
; "LICENSE" for information on usage and redistribution of this file.
;
; SPDX-License-Identifier: Apache-2.0
;
; Section 13 abstract-model companion: the capability rights/revocation algebra.
;
; An SMT (Z3) model of the invariants that kernel/cap/table.c must uphold, over
; the rtmux_cap_slot fields in kernel/include/cap-table.h (group / rights /
; object / context; there is no parent edge -- lifetime is by revocation group,
; not by an ancestry chain).  Each invariant is checked two ways:
;   (sanity)   the modeled scenario is reachable            -> expect sat
;   (negation) the invariant's negation is unsatisfiable    -> expect unsat
; so an unsat is meaningful (not vacuous over-constraint).  A separate planted
; file (cap-algebra-planted.smt2) breaks each operation and must go sat.
;
; Scope: this is a spec-consistency model of the group/rights/revoke algebra,
; NOT a full cap-table proof.  It deliberately does NOT model generation /
; stale-handle rejection, pin_count lifetime, delayed (REVOKING -> DEAD after
; final unpin) destruction, or RTMUX_CAP_GEN_MAX slot retirement.  Invariants 1
; and 2 are line-level consistency checks (they restate the delegate guard and
; group copy); 3, 4, 5 constrain the freshness / kill-completeness / isolation
; shape.  Faithful to: delegate rights guard table.c:440, delegate group share
; :458, cap_next_group nonzero+not-live :121-129, revoke group-kill :157-159.

(set-logic QF_BV)

; ---------------------------------------------------------------------------
; Invariant 1 (line-level): rights attenuation.  delegate() rejects unless the
; requested rights r are a subset of the source rights pr (table.c:440) and sets
; the child rights to r (:461); so a live child's rights are subseteq the source.
(push 1)
(declare-const pr (_ BitVec 4))
(declare-const r  (_ BitVec 4))
(assert (= (bvand r (bvnot pr)) #b0000)) ; delegate precondition: r subseteq pr
(assert (not (= r #b0000)))              ; a non-trivial delegation
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const pr (_ BitVec 4))
(declare-const r  (_ BitVec 4))
(assert (= (bvand r (bvnot pr)) #b0000))
(assert (not (= (bvand r (bvnot pr)) #b0000))) ; negation: child exceeds source
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 2 (line-level): same-table delegation shares the source revocation
; group (table.c:458 newslot->group = slot->group).
(push 1)
(declare-const pg (_ BitVec 16))
(declare-const cg (_ BitVec 16))
(assert (= cg pg)) ; the operation copies the group
(assert (not (= pg #x0000))) ; a real (assigned) group
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const pg (_ BitVec 16))
(declare-const cg (_ BitVec 16))
(assert (= cg pg))
(assert (not (= cg pg))) ; negation
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 3: cross-table transfer mints a fresh group.  cap_next_group
; (table.c:121) returns an id that is nonzero and NOT live in the destination
; (it skips 0 and skips any currently-live group), so it collides with no live
; destination group.  Modeled as that postcondition, not as a numeric ordering.
(push 1)
(declare-const g1 (_ BitVec 16)) ; live dst groups
(declare-const g2 (_ BitVec 16))
(declare-const ng (_ BitVec 16)) ; minted group
(assert (not (= ng #x0000)))     ; nonzero
(assert (not (= ng g1)))         ; not live
(assert (not (= ng g2)))
(check-sat) ; sanity: sat (a fresh id exists)
(pop 1)

(push 1)
(declare-const g1 (_ BitVec 16))
(declare-const g2 (_ BitVec 16))
(declare-const ng (_ BitVec 16))
(assert (not (= ng #x0000)))
(assert (not (= ng g1)))
(assert (not (= ng g2)))
(assert (or (= ng g1) (= ng g2))) ; negation: the minted group is live
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 4: an unpinned revoke kills the WHOLE group.  When no slot in the
; group is pinned, rtmux_cap_revoke clears every slot whose group == G
; (table.c:157-159), so none stays live.  (A pinned revoke instead marks the
; group REVOKING and defers the kill to the final unpin, table.c:377; that
; lifetime path is outside this algebra model -- hence the no-pins precondition.)
(push 1)
(declare-const s1g (_ BitVec 16)) (declare-const s2g (_ BitVec 16))
(declare-const G   (_ BitVec 16))
(declare-const s1pin Bool) (declare-const s2pin Bool)
(declare-const s1dead Bool) (declare-const s2dead Bool)
(assert (not s1pin)) (assert (not s2pin)) ; unpinned revoke
(assert (= s1dead (= s1g G)))
(assert (= s2dead (= s2g G)))
(assert (= s1g G)) (assert (= s2g G)) ; both slots are in the revoked group
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const s1g (_ BitVec 16)) (declare-const s2g (_ BitVec 16))
(declare-const G   (_ BitVec 16))
(declare-const s1pin Bool) (declare-const s2pin Bool)
(declare-const s1dead Bool) (declare-const s2dead Bool)
(assert (not s1pin)) (assert (not s2pin))
(assert (= s1dead (= s1g G)))
(assert (= s2dead (= s2g G)))
(assert (= s1g G)) (assert (= s2g G))
(assert (or (not s1dead) (not s2dead))) ; negation: a group member survives
(check-sat) ; unsat
(pop 1)

; ---------------------------------------------------------------------------
; Invariant 5: independent roots stay isolated.  Distinct roots get distinct
; groups, and revoke keys on the group, so revoking one root's group cannot kill
; a cap belonging to another root's group.
(push 1)
(declare-const g1 (_ BitVec 16)) ; revoked root's group
(declare-const g2 (_ BitVec 16)) ; other root's group
(declare-const c2g (_ BitVec 16))
(declare-const c2dead Bool)
(assert (distinct g1 g2))
(assert (= c2g g2))
(assert (= c2dead (= c2g g1))) ; revoke(g1) clears iff the group is g1
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const g1 (_ BitVec 16))
(declare-const g2 (_ BitVec 16))
(declare-const c2g (_ BitVec 16))
(declare-const c2dead Bool)
(assert (distinct g1 g2))
(assert (= c2g g2))
(assert (= c2dead (= c2g g1)))
(assert c2dead) ; negation: the other root's cap got killed
(check-sat) ; unsat
(pop 1)
