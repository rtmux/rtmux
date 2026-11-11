; RTMux is freely redistributable under the Apache License. See the file
; "LICENSE" for information on usage and redistribution of this file.
;
; SPDX-License-Identifier: Apache-2.0
;
; Teeth control for lifecycle.smt2.  The region between BEGIN-SHARED and
; END-SHARED is the model's state, operations and invariants, byte-identical to
; lifecycle.smt2 (scripts/check-lifecycle-smt.sh compares them).  Below it, each
; block breaks one operation and must be sat.

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

; Each block replaces ONE modeled operation by a broken variant, starting from a
; state satisfying the whole invariant, so the matching invariant's negation
; becomes satisfiable.  The gate requires every block here to be sat, one per
; invariant (9), and the shared region above to match lifecycle.smt2.

; P1 broken: reap skips the channel sweep, so a dead process still owns channels.
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p))
(assert (and (= (st p) 1) (= (ncap p) 0) (= (ncli p) 0) (= (nsrv p) 0)
             (= q (mkP 2 (nthr p) (nchan p) (ncap p) (nfds p) (ncli p) (nsrv p)))))
(assert (not (P1 q)))
(check-sat) ; expect sat
(pop 1)

; P2 broken: zombify without waiting for the last thread (the zombie keeps a thread).
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p))
(assert (and (= (st p) 0)
             (= q (mkP 1 (nthr p) (nchan p) (ncap p) (nfds p) (ncli p) (nsrv p)))))
(assert (not (P2 q)))
(check-sat) ; expect sat
(pop 1)

; P3 broken: a server-side scoid slot bumps nfds like a client attach.
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p))
(assert (and (= (st p) 0)
             (= q (mkP 0 (nthr p) (nchan p) (ncap p) (+ (nfds p) 1) (ncli p) (+ (nsrv p) 1)))))
(assert (not (P3 q)))
(check-sat) ; expect sat
(pop 1)

; P4 broken: a second client detach without the slot-exists guard drives nfds
; negative (the uint16 wrap-to-65535 of 9717d81c).
(push 1)
(declare-const p P) (declare-const q P)
(assert (p_inv p))
(assert (and (< (st p) 2)
             (= q (mkP (st p) (nthr p) (nchan p) (ncap p) (- (nfds p) 1) (- (ncli p) 1) (nsrv p)))))
(assert (not (P4 q)))
(check-sat) ; expect sat
(pop 1)

; C1 broken: connect_links_drop with no holder (double drop) underflows links.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c))
(assert (and (= (alive c) 1)
             (= d (mkC (ite (> (links c) 1) 1 0) (- (links c) 1) (- (cref c) 1)
                       (infl c) (bst c) (bon c) (cown c) (town c) (tgen c) (rgen c) (rlive c)))))
(assert (not (C1 d)))
(check-sat) ; expect sat
(pop 1)

; C2 broken: a plain (lost) increment: the client slot is added but links is not.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c))
(assert (and (= (alive c) 1)
             (= d (mkC 1 (links c) (+ (cref c) 1) (infl c) (bst c) (bon c)
                       (cown c) (town c) (tgen c) (rgen c) (rlive c)))))
(assert (not (C2 d)))
(check-sat) ; expect sat
(pop 1)

; C3 broken: the sender blocks and takes its link but never records blocked_on.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c))
(assert (and (= (alive c) 1) (= (bst c) 0) (= (town c) (cown c))
             (= d (mkC 1 (+ (links c) 1) (cref c) 1 1 0 (cown c) (town c) (tgen c) (rgen c)
                       (rlive c)))))
(assert (not (C3 d)))
(check-sat) ; expect sat
(pop 1)

; C4 broken: teardown drops the last link and frees without clearing blocked_on
; (the 3607e29a use-after-free).
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c))
(assert (and (= (alive c) 1) (= (bst c) 1)
             (= d (mkC (ite (> (links c) 1) 1 0) (- (links c) 1) (cref c) 0 0 (bon c)
                       (cown c) (town c) (tgen c) (rgen c) 0))))
(assert (not (C4 d)))
(check-sat) ; expect sat
(pop 1)

; C5 broken: the thread is recycled (new generation) while a receiver still
; holds its rcvid, so the rcvid names a stale generation.
(push 1)
(declare-const c C) (declare-const d C)
(assert (c_inv c))
(assert (= d (mkC (alive c) (links c) (cref c) (infl c) (bst c) (bon c) (cown c) (town c)
                  (+ (tgen c) 1) (rgen c) (rlive c))))
(assert (not (C5 d)))
(check-sat) ; expect sat
(pop 1)
