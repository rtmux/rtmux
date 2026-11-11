; RTMux is freely redistributable under the Apache License. See the file
; "LICENSE" for information on usage and redistribution of this file.
;
; SPDX-License-Identifier: Apache-2.0
;
; Section 13 abstract-model companion: object-lifecycle teardown invariants.
;
; An SMT (Z3) transition-system model of the process / connection / blocked-
; thread lifecycle, written as contract capture for three teardown hazards that
; were fixed once:
;   9717d81c  process reap saw nfds != 0: nfds was decremented without the
;             lock that attach used, so an update was lost or wrapped (uint16).
;   95b4205a  connect_t.links was a refcount mutated by inconsistent plain and
;             atomic updates, so a drop could be lost or underflow.
;   3607e29a  thread_t.blocked_on still named a connect_t that teardown freed
;             (use-after-free in ksys_msg_readv).
; Code boundary: kernel/ext/process.c (reap guards, nfds check), kernel/ipc/
; connect.c (connect_attach nfds++, process_nfds_dec, connect_detach), kernel/
; ipc/channel.c clear_msgxfer, kernel/sched/dispatch.c force_ready, and
; kernel/object/lookup.c lookup_rcvid (rcvid = scoid + tid, the thread looked up
; in cop->process->threads).
;
; Method: each invariant is inductive.  An invariant is "unsat" when no state
; satisfying the whole invariant conjunction Inv has a single modeled step to a
; state violating it.  Each invariant also has a witness query (a step reaching
; the interesting state -> sat), and each of the two sub-models has a base case
; (the initial state satisfies Inv, which also shows Inv is not vacuously
; unsatisfiable) and a concrete multi-step trace from the initial state (sat),
; so the witnesses are reachable and not only Inv-consistent.
; tests/smt/lifecycle-planted.smt2 breaks one operation per invariant and must
; go sat.
;
; Scope: this is a CONTRACT model of the invariants, NOT a live-race gate.  It
; is sequential: refcount atomicity, RCU/SMR lifetime and memory ordering stay
; with CBMC, the litmus tests and runtime tests.  It models ONE process and ONE
; connection with ONE blocked sender (the nfds, links and blocked_on relations
; are per-object, so one instance is the contract; no cross-object property is
; claimed).  Reap guards are modeled as the postcondition of the teardown sweeps
; (the sweeps themselves are not modeled).  Not modeled: global channels,
; netcon connections, the DISCONNECT path that nulls cop->process, event
; delivery, and the vector/slot representation.
;
; Model-to-code gaps (reviewed against the code, kept honest):
;  - c_refDrop (last drop frees) models the process-exit / teardown drops
;    (_connect_detach, connect_links_drop callers).  The client ConnectDetach
;    kcall path (ksys_connect_detach) does NOT free on the last drop, and
;    links == 0 marks a server connect in the code; neither is modeled.
;  - The rcvid generation (tgen / rgen) is a model abstraction: lookup_rcvid
;    checks no generation, its callers validate thread state.  C5 states the
;    contract those caller checks must provide, not a property of lookup_rcvid.
;  - P3, P4 and C4 hold because the sequential steps change the related fields
;    together (and the slot-exists guards stand for CRASHCHECK(nfds == 0) and
;    links underflow crashes).  The lost-update race of 9717d81c and the
;    clear-then-drop ordering race of 3607e29a are not representable; the
;    model records the intended end states, the mutations show that violating
;    them is detectable.
;  - c_create leaves the owner process unconstrained (a fresh connection).
;
; The region between BEGIN-SHARED and END-SHARED is byte-identical in the
; planted file; the gate compares the two.

(set-logic ALL)

;; BEGIN-SHARED
; ---- Process sub-model.  st: 0 live, 1 zombie (all threads gone, teardown
; sweeps running), 2 dead (reaped).  ncli = client-side connection slots in
; fdcons, nsrv = server-side scoid slots in chancons; nfds counts only client
; attaches.
(declare-datatypes ((P 0))
  (((mkP (st Int) (nthr Int) (nchan Int) (ncap Int) (nfds Int) (ncli Int)
         (nsrv Int)))))

(define-fun p_init ((p P)) Bool (= p (mkP 0 0 0 0 0 0 0)))

; connect_attach: client attach bumps nfds; a server-side scoid slot does not.
(define-fun p_attachC ((p P) (q P)) Bool
  (and (= (st p) 0)
       (= q (mkP 0 (nthr p) (nchan p) (ncap p) (+ (nfds p) 1) (+ (ncli p) 1) (nsrv p)))))
(define-fun p_attachS ((p P) (q P)) Bool
  (and (= (st p) 0)
       (= q (mkP 0 (nthr p) (nchan p) (ncap p) (nfds p) (ncli p) (+ (nsrv p) 1)))))
; process_nfds_dec runs once per client reference dropped; a server-side slot
; drop does not touch nfds.
(define-fun p_detachC ((p P) (q P)) Bool
  (and (< (st p) 2) (> (ncli p) 0)
       (= q (mkP (st p) (nthr p) (nchan p) (ncap p) (- (nfds p) 1) (- (ncli p) 1) (nsrv p)))))
(define-fun p_detachS ((p P) (q P)) Bool
  (and (< (st p) 2) (> (nsrv p) 0)
       (= q (mkP (st p) (nthr p) (nchan p) (ncap p) (nfds p) (ncli p) (- (nsrv p) 1)))))
; Threads, channels and caps are created only while live; any may drain later.
(define-fun p_thrNew ((p P) (q P)) Bool
  (and (= (st p) 0)
       (= q (mkP 0 (+ (nthr p) 1) (nchan p) (ncap p) (nfds p) (ncli p) (nsrv p)))))
(define-fun p_thrExit ((p P) (q P)) Bool
  (and (< (st p) 2) (> (nthr p) 0)
       (= q (mkP (st p) (- (nthr p) 1) (nchan p) (ncap p) (nfds p) (ncli p) (nsrv p)))))
(define-fun p_chanNew ((p P) (q P)) Bool
  (and (= (st p) 0)
       (= q (mkP 0 (nthr p) (+ (nchan p) 1) (ncap p) (nfds p) (ncli p) (nsrv p)))))
(define-fun p_chanDel ((p P) (q P)) Bool
  (and (< (st p) 2) (> (nchan p) 0)
       (= q (mkP (st p) (nthr p) (- (nchan p) 1) (ncap p) (nfds p) (ncli p) (nsrv p)))))
(define-fun p_capNew ((p P) (q P)) Bool
  (and (= (st p) 0)
       (= q (mkP 0 (nthr p) (nchan p) (+ (ncap p) 1) (nfds p) (ncli p) (nsrv p)))))
(define-fun p_capDel ((p P) (q P)) Bool
  (and (< (st p) 2) (> (ncap p) 0)
       (= q (mkP (st p) (nthr p) (nchan p) (- (ncap p) 1) (nfds p) (ncli p) (nsrv p)))))
; The last thread is gone: the process becomes a zombie.
(define-fun p_zombify ((p P) (q P)) Bool
  (and (= (st p) 0) (= (nthr p) 0)
       (= q (mkP 1 (nthr p) (nchan p) (ncap p) (nfds p) (ncli p) (nsrv p)))))
; Reap (process destroy): runs after the teardown sweeps drained every channel,
; cap and connection slot; the code then crash()es unless nfds == 0, so nfds is
; NOT in this guard -- it must follow from the invariant.
(define-fun p_reap ((p P) (q P)) Bool
  (and (= (st p) 1) (= (nchan p) 0) (= (ncap p) 0) (= (ncli p) 0) (= (nsrv p) 0)
       (= q (mkP 2 (nthr p) (nchan p) (ncap p) (nfds p) (ncli p) (nsrv p)))))

(define-fun p_step ((p P) (q P)) Bool
  (or (p_attachC p q) (p_attachS p q) (p_detachC p q) (p_detachS p q)
      (p_thrNew p q) (p_thrExit p q) (p_chanNew p q) (p_chanDel p q)
      (p_capNew p q) (p_capDel p q) (p_zombify p q) (p_reap p q)))

; P1: a dead process holds no threads, channels, caps or connections.
(define-fun P1 ((p P)) Bool
  (=> (= (st p) 2)
      (and (= (nthr p) 0) (= (nchan p) 0) (= (ncap p) 0)
           (= (nfds p) 0) (= (ncli p) 0) (= (nsrv p) 0))))
; P2: a zombie has zero threads.
(define-fun P2 ((p P)) Bool (=> (>= (st p) 1) (= (nthr p) 0)))
; P3: nfds counts exactly the client attaches; server-side slots are excluded.
(define-fun P3 ((p P)) Bool (= (nfds p) (ncli p)))
; P4: every count is non-negative (no uint16 wrap-to-65535 from a double drop).
(define-fun P4 ((p P)) Bool
  (and (>= (nthr p) 0) (>= (nchan p) 0) (>= (ncap p) 0) (>= (nfds p) 0)
       (>= (ncli p) 0) (>= (nsrv p) 0)))
(define-fun p_inv ((p P)) Bool
  (and (>= (st p) 0) (<= (st p) 2) (P1 p) (P2 p) (P3 p) (P4 p)))

; ---- Connection / blocked-sender sub-model.  alive: connect_t published and
; not freed.  links = connect_t.links; cref = client slots holding a link;
; infl = in-flight senders holding a link.  bst: the one modeled thread is
; blocked in Send (1) or ready (0); bon = 1 iff its blocked_on names the
; connection.  cown / town: owner process of the connection and of the thread;
; tgen / rgen: thread generation and the generation captured by an rcvid;
; rlive: a receiver holds an rcvid for the thread.
(declare-datatypes ((C 0))
  (((mkC (alive Int) (links Int) (cref Int) (infl Int) (bst Int) (bon Int)
         (cown Int) (town Int) (tgen Int) (rgen Int) (rlive Int)))))

(define-fun c_init ((c C)) Bool (= c (mkC 0 0 0 0 0 0 0 0 0 0 0)))

; A fresh connection with its first client slot (connect_attach, soul alloc).
(define-fun c_create ((c C) (d C)) Bool
  (and (= (alive c) 0)
       (= d (mkC 1 1 1 0 (bst c) (bon c) (cown d) (town c) (tgen c) (rgen c) (rlive c)))))
; Another client slot shares it: atomic links++.
(define-fun c_refAdd ((c C) (d C)) Bool
  (and (= (alive c) 1)
       (= d (mkC 1 (+ (links c) 1) (+ (cref c) 1) (infl c) (bst c) (bon c)
                 (cown c) (town c) (tgen c) (rgen c) (rlive c)))))
; A client slot drops its link; the last drop frees (connect_links_drop).
(define-fun c_refDrop ((c C) (d C)) Bool
  (and (= (alive c) 1) (> (cref c) 0)
       (= d (mkC (ite (> (links c) 1) 1 0) (- (links c) 1) (- (cref c) 1)
                 (infl c) (bst c) (bon c) (cown c) (town c) (tgen c) (rgen c) (rlive c)))))
; The thread blocks in Send on its own process's connection: it takes a link and
; sets blocked_on.
(define-fun c_block ((c C) (d C)) Bool
  (and (= (alive c) 1) (= (bst c) 0) (= (town c) (cown c))
       (= d (mkC 1 (+ (links c) 1) (cref c) 1 1 1 (cown c) (town c) (tgen c) (rgen c)
                 (rlive c)))))
; The server receives: the rcvid captures the sender's generation.
(define-fun c_recv ((c C) (d C)) Bool
  (and (= (bst c) 1) (= (rlive c) 0)
       (= d (mkC (alive c) (links c) (cref c) (infl c) 1 (bon c) (cown c) (town c)
                 (tgen c) (tgen c) 1))))
; Reply or teardown (clear_msgxfer / force_ready STATE_SEND and STATE_REPLY):
; blocked_on is cleared BEFORE the in-flight link is dropped, and the rcvid dies.
(define-fun c_unblock ((c C) (d C)) Bool
  (and (= (alive c) 1) (= (bst c) 1)
       (= d (mkC (ite (> (links c) 1) 1 0) (- (links c) 1) (cref c) 0 0 0
                 (cown c) (town c) (tgen c) (rgen c) 0))))
; A ready thread is recycled into another process: a new generation.
(define-fun c_recycle ((c C) (d C)) Bool
  (and (= (bst c) 0)
       (= d (mkC (alive c) (links c) (cref c) (infl c) 0 (bon c) (cown c) (town d)
                 (+ (tgen c) 1) (rgen c) (rlive c)))))

(define-fun c_step ((c C) (d C)) Bool
  (or (c_create c d) (c_refAdd c d) (c_refDrop c d) (c_block c d) (c_recv c d)
      (c_unblock c d) (c_recycle c d)))

; C1: refcounts are non-negative.
(define-fun C1 ((c C)) Bool
  (and (>= (links c) 0) (>= (cref c) 0) (>= (infl c) 0)))
; C2: links equals the number of holders; a freed connection has none.
(define-fun C2 ((c C)) Bool
  (and (= (links c) (+ (cref c) (infl c)))
       (=> (= (alive c) 0) (= (links c) 0))
       (=> (= (alive c) 1) (> (links c) 0))))
; C3: a thread is blocked on at most one object: blocked_on is set iff the
; thread is blocked, and a blocked sender holds exactly one in-flight link.
(define-fun C3 ((c C)) Bool
  (and (or (= (bst c) 0) (= (bst c) 1)) (= (bon c) (bst c)) (= (infl c) (bst c))))
; C4: blocked_on never names a freed connection (the 3607e29a use-after-free).
(define-fun C4 ((c C)) Bool (=> (= (bon c) 1) (= (alive c) 1)))
; C5: owner and rcvid pairing: a blocked sender belongs to the connection's
; owner process, and a live rcvid names a blocked sender of the same
; generation.
(define-fun C5 ((c C)) Bool
  (and (=> (= (bst c) 1) (= (town c) (cown c)))
       (=> (= (rlive c) 1) (and (= (bst c) 1) (= (rgen c) (tgen c))))
       (or (= (rlive c) 0) (= (rlive c) 1))))
(define-fun c_inv ((c C)) Bool
  (and (or (= (alive c) 0) (= (alive c) 1)) (C1 c) (C2 c) (C3 c) (C4 c) (C5 c)))
;; END-SHARED

; ===========================================================================
; Base cases: the initial states satisfy the invariant (also shows Inv is
; satisfiable), and a concrete reachable trace to the interesting end state.
(push 1)
(declare-const p P)
(assert (p_init p))
(assert (p_inv p))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const p P)
(assert (p_init p))
(assert (not (p_inv p))) ; negation
(check-sat) ; unsat
(pop 1)

(push 1)
(declare-const c C)
(assert (c_init c))
(assert (c_inv c))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const c C)
(assert (c_init c))
(assert (not (c_inv c))) ; negation
(check-sat) ; unsat
(pop 1)

; Reachable trace, process: thread, channel, client attach, server slot, thread
; exits, zombify, sweeps drain, reap -> dead with nfds 0.
(push 1)
(declare-const s0 P) (declare-const s1 P) (declare-const s2 P) (declare-const s3 P)
(declare-const s4 P) (declare-const s5 P) (declare-const s6 P) (declare-const s7 P)
(declare-const s8 P) (declare-const s9 P) (declare-const s10 P)
(assert (and (p_init s0) (p_thrNew s0 s1) (p_chanNew s1 s2) (p_attachC s2 s3)
             (p_attachS s3 s4) (p_thrExit s4 s5) (p_zombify s5 s6)
             (p_detachC s6 s7) (p_detachS s7 s8) (p_chanDel s8 s9) (p_reap s9 s10)))
(assert (and (= (st s10) 2) (= (nfds s3) 1) (= (nfds s4) 1) (= (nfds s10) 0)))
(check-sat) ; sanity: sat
(pop 1)

; Reachable trace, connection: create, second ref, block, receive, reply, both
; refs dropped -> freed.
(push 1)
(declare-const t0 C) (declare-const t1 C) (declare-const t2 C) (declare-const t3 C)
(declare-const t4 C) (declare-const t5 C) (declare-const t6 C) (declare-const t7 C)
(assert (and (c_init t0) (c_create t0 t1) (c_refAdd t1 t2) (c_block t2 t3)
             (c_recv t3 t4) (c_unblock t4 t5) (c_refDrop t5 t6) (c_refDrop t6 t7)))
(assert (and (= (bon t3) 1) (= (rlive t4) 1) (= (bon t5) 0) (= (alive t7) 0)))
(check-sat) ; sanity: sat
(pop 1)

; ===========================================================================
; P1: dead process holds nothing.  Witness: a step that reaps.
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_step p q)) (assert (= (st q) 2))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_step p q)) (assert (not (P1 q)))
(check-sat) ; unsat
(pop 1)

; P2: zombie has zero threads.  Witness: a step that zombifies.
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_step p q)) (assert (= (st q) 1))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_step p q)) (assert (not (P2 q)))
(check-sat) ; unsat
(pop 1)

; P3: nfds = client attaches only.  Witness: server slots exist alongside
; client slots and nfds still equals the client count.
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_step p q))
(assert (> (nsrv q) 0)) (assert (> (nfds q) 0))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_step p q)) (assert (not (P3 q)))
(check-sat) ; unsat
(pop 1)

; P4: no count underflows.  Witness: a detach down to zero.
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_detachC p q)) (assert (= (nfds q) 0))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p)) (assert (p_step p q)) (assert (not (P4 q)))
(check-sat) ; unsat
(pop 1)

; C1: refcounts non-negative.  Witness: a drop to zero (the last drop).
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_refDrop c d)) (assert (= (links d) 0))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_step c d)) (assert (not (C1 d)))
(check-sat) ; unsat
(pop 1)

; C2: links = holders.  Witness: a connection shared by a client slot and an
; in-flight sender.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_step c d))
(assert (> (cref d) 0)) (assert (= (infl d) 1)) (assert (= (links d) 2))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_step c d)) (assert (not (C2 d)))
(check-sat) ; unsat
(pop 1)

; C3: blocked on at most one object.  Witness: a blocked sender.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_block c d)) (assert (= (bon d) 1))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_step c d)) (assert (not (C3 d)))
(check-sat) ; unsat
(pop 1)

; C4: blocked_on never dangles.  Witness: the last link is dropped (freed) by
; an unblock, with blocked_on already cleared.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_unblock c d)) (assert (= (alive d) 0)) (assert (= (bon d) 0))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_step c d)) (assert (not (C4 d)))
(check-sat) ; unsat
(pop 1)

; C5: owner and rcvid pairing.  Witness: a live rcvid for a blocked sender.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_recv c d)) (assert (= (rlive d) 1)) (assert (= (town d) (cown d)))
(check-sat) ; sanity: sat
(pop 1)

(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c)) (assert (c_step c d)) (assert (not (C5 d)))
(check-sat) ; unsat
(pop 1)
