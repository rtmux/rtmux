; RTMux is freely redistributable under the Apache License. See the file
; "LICENSE" for information on usage and redistribution of this file.
;
; SPDX-License-Identifier: Apache-2.0
;
; Teeth control for cap-algebra.smt2.  Each block models a BROKEN version of one
; capability operation, so the matching invariant's negation becomes
; satisfiable.  The gate requires every block here to be sat, one per invariant:
; if any were unsat, the corresponding real check would be passing vacuously.

(set-logic QF_BV)

; Invariant 1 broken: delegate drops the table.c:440 attenuation guard, so a
; child may hold rights the source never had.
(push 1)
(declare-const pr (_ BitVec 4))
(declare-const r  (_ BitVec 4))
(assert (not (= (bvand r (bvnot pr)) #b0000))) ; child exceeds source rights
(check-sat) ; expect sat
(pop 1)

; Invariant 2 broken: same-table delegation mints a DIFFERENT group instead of
; copying the source's, so the shared-group property fails.
(push 1)
(declare-const pg (_ BitVec 16))
(declare-const cg (_ BitVec 16))
(assert (distinct cg pg)) ; bug: child group differs from source
(check-sat) ; expect sat
(pop 1)

; Invariant 3 broken: cross-table transfer copies the source group, which is
; already live in the destination, instead of minting a fresh one.
(push 1)
(declare-const g1 (_ BitVec 16)) ; a live dst group
(declare-const ng (_ BitVec 16)) ; the "minted" group
(assert (= ng g1)) ; bug: collides with a live destination group
(check-sat) ; expect sat
(pop 1)

; Invariant 4 broken: revoke clears only the first matching slot, leaving
; another member of the same group live.
(push 1)
(declare-const s1g (_ BitVec 16)) (declare-const s2g (_ BitVec 16))
(declare-const G   (_ BitVec 16))
(declare-const s1dead Bool) (declare-const s2dead Bool)
(assert (= s1dead (= s1g G)))
(assert (= s2dead false)) ; bug: second slot never cleared
(assert (= s1g G)) (assert (= s2g G)) ; both in the revoked group
(assert (or (not s1dead) (not s2dead))) ; a member survives
(check-sat) ; expect sat
(pop 1)

; Invariant 5 broken: revoke keys on the OBJECT identity instead of the group,
; so an unrelated root that happens to name the same object is killed.
(push 1)
(declare-const c2g   (_ BitVec 16)) (declare-const g1 (_ BitVec 16))
(declare-const c2obj (_ BitVec 16)) (declare-const robj (_ BitVec 16))
(declare-const c2dead Bool)
(assert (distinct c2g g1)) ; the cap is in a different revocation group
(assert (= c2obj robj))    ; but names the same object as the revoked cap
(assert (= c2dead (= c2obj robj))) ; bug: dead keyed on object, not group
(assert c2dead) ; the unrelated root's cap got killed
(check-sat) ; expect sat
(pop 1)
