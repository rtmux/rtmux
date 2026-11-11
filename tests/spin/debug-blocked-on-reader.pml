/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Q85 diagnostic: lock-free debug_thread() reader vs blocked_on publication.
 *
 * Reader (kernel/debug/thread-debug.c, debug_thread, reached from
 * kernel/ext/debug.c RTMUX_DEBUG_THREAD_INFO): the target thread is pinned,
 * not stopped (`stopped` is computed there but never gates the call).  It does
 *     switch (dtp->state = thp->state)         load 1, plain
 *       STATE_SEND/REPLY: cop = thp->blocked_on        load 2, plain, used as
 *                         connect_t, cop->flags with no NULL check
 *       STATE_RECEIVE:    ((channel_t *) thp->blocked_on)->chid, no NULL check
 * Two unordered loads, no WAIT_PENDING spin, no lock, no re-validation.  The
 * SEM/MUTEX arms of the same switch DO test blocked_on for NULL.
 *
 * blocked_on holds different types depending on the writer's phase, so a
 * reader keyed only on `state` can see a pointer of the wrong type or NULL.
 * One scenario macro per source writer; each must produce its own tagged
 * counterexample:
 *
 *   -DSC_PUBLISH  LOCAL reply-block of a sender (fastmsg.c, ksys_msg_sendv,
 *                 receiver already waiting).  The source writes
 *                 `sender->state = STATE_REPLY` DIRECTLY (not via unready()),
 *                 then block_and_ready(thp), which does not touch the sender's
 *                 blocked_on, and only afterwards `sender->blocked_on = cop`.
 *                 Nothing clears blocked_on in this path, so the (REPLY, NULL)
 *                 window exists ONLY IF the sender's previous blocked_on was
 *                 already NULL; this model assumes that (PRE_B = NULL) and does
 *                 not claim a stale non-NULL previous value.  Reader sees
 *                 (REPLY, NULL).                         tag: debug_null_deref
 *   -DSC_PUBSEND  LOCAL send-block (fastmsg.c, ksys_msg_sendv, no receiver):
 *                 unready(sender, STATE_SEND) sets STATE_SEND and its block()
 *                 clears blocked_on to NULL for this state (dispatch.c block()
 *                 default arm), so the NULL is unconditional; only afterwards
 *                 `sender->blocked_on = cop`.  Reader sees (SEND, NULL).
 *                                                        tag: debug_null_deref
 *   -DSC_COMPLETE reply/force_ready completion (ksys_msg_replyv,
 *                 force_ready): `blocked_on = NULL` under chp->lock, state
 *                 stays STATE_REPLY until ready().  Reader sees (REPLY, NULL).
 *                                                        tag: debug_null_deref
 *   -DSC_RECV     REMOTE (COF_NETCON) only: remote_ipc_msg() in
 *                 message-core.c finds a waiting receiver and does
 *                 `atomic_or(flags, SHORT_MSG); thp->blocked_on = act` with thp
 *                 still STATE_RECEIVE until block_and_ready().  A local send
 *                 does NOT take this path (it uses START_SMP_XFER, which writes
 *                 the ACTIVE thread's blocked_on, not a RECEIVE-state target's).
 *                 Source order is flag first, THEN the pointer.  Reader sees
 *                 (RECEIVE, thread) and reads a thread_t as channel_t.
 *                                                        tag: debug_cross_type
 *   -DSC_SELF     short reply to an as_prp client (ksys_msg_replyv):
 *                 `thp->blocked_on = thp` (self pointer; the client's special
 *                 return uses thp == thp2 to mean "reply") while the client is
 *                 still STATE_REPLY.  Reader sees (REPLY, self) and reads a
 *                 thread_t as connect_t.                 tag: debug_cross_type
 *
 *   -DSC_RECVPUB  receiver blocks (fastmsg.c, receive path, "Step 5/6" comment):
 *                 unready(act, STATE_RECEIVE) sets state and block() zeroes
 *                 blocked_on; only afterwards `act->blocked_on = chp`.  The
 *                 WAIT_PENDING bridge covers wakers, but debug_thread() does
 *                 not spin on it.  Reader sees (RECEIVE, NULL).
 *                                                        tag: debug_null_deref
 *
 * Controls (abstract; NOT proposed fixes, no such change exists in the tree).
 * A clean result is only meaningful if the run could have failed, so each
 * clean control has a paired check that the same model is able to fail:
 *   -DSERIAL_CTRL reader's two loads and the whole writer sequence are
 *                 mutually exclusive.  Expected: no violation.
 *   -DPROBE_PRE / -DPROBE_POST  (with SERIAL_CTRL) assert that the reader can
 *                 NOT observe the pre-/post-writer state.  Both must fail,
 *                 proving the serial control still explores both orders and is
 *                 not clean merely because the reader never sees anything.
 *   -DWRITER_SPLIT  reader pair stays atomic but the writer's stores are locked
 *                 one by one.  Expected: the SAME violation as the baseline,
 *                 showing that the exclusion must span the whole writer
 *                 sequence, i.e. the intermediate state is what matters.
 *   -DGUARD_NULL  reader tolerates NULL, as the SEM/MUTEX arms already do.
 *                 Expected: removes the NULL scenarios and does NOT remove
 *                 the cross-type scenarios (a NULL test says nothing about
 *                 the pointee's type).
 * None of these is evidence that a given kernel change would be correct.
 *
 * Scope and assumptions: sequentially consistent (SC) interleaving of single
 * loads and stores.  RVWMO is not modelled: the reader's two plain loads and
 * the writer's stores may also be reordered by hardware, so real outcomes are a
 * superset of what is found here, and a clean SC result would NOT imply safety
 * on RISC-V.  Only the reader/writer pair is modelled.  It does not model
 * the stall detector (stall-detector.c re-reads state and timestamp_last_block
 * around the typing, which this model has no timestamp for), the special-return
 * consumer of the self pointer (where thp == thp2 is deliberate), pin/RCU
 * lifetime of the pointee, or any writer-writer race, so it does NOT explain
 * the special-return.c null-blocked_on crash in TODO Q67 item 4.  It shows
 * type/NULL exposure of a state-keyed debug reader, not that any run has
 * crashed; consequences of a cross-type read (garbage field or wild pointer)
 * depend on layouts that are not modelled.  The reader's expected-type table
 * is a simplification of the switch in debug_thread().  Reachability of each
 * source interleaving depends on the callers; this model assumes it.
 *
 * SMP requirement: the reader (a debug kcall) must run on a different hart from
 * the writer while the target is mid-transition, so every scenario needs
 * CONFIG_PROCESSORS_MAX > 1.  With one processor INLINE_BLOCKANDREADY is used
 * and the two sides cannot interleave.  Writers deliberately NOT modelled:
 * STATE_JOIN/WAITTHREAD (thread.c:478, lifecycle.c:1275 `blocked_on = thp`; the
 * JOIN arm also has no NULL check but is a separate analysis), SEM/MUTEX/
 * CONDVAR (their arms test NULL), START_SMP_XFER/END_SMP_XFER and the
 * message.c/xfer clears (they write the ACTIVE thread's blocked_on while it is
 * RUNNING, which the reader's switch ignores), and force_ready/teardown
 * clears other than COMPLETE.
 */

#define ST_RUN     0
#define ST_SEND    1
#define ST_REPLY   2
#define ST_RECEIVE 3
#define ST_READY   4

#define B_NULL 0
#define B_CONN 1   /* connect_t * */
#define B_CHAN 2   /* channel_t * */
#define B_THR  3   /* another thread_t * (the sender) */
#define B_SELF 4   /* the thread itself */
#define B_ANY  5

/* PRE_*: state before the writer runs; POST_*: after it finishes. */
#if defined(SC_PUBLISH)
#define PRE_S ST_RUN
#define PRE_B B_NULL
#define POST_S ST_REPLY
#define POST_B B_CONN
#elif defined(SC_PUBSEND)
#define PRE_S ST_RUN
#define PRE_B B_NULL
#define POST_S ST_SEND
#define POST_B B_CONN
#elif defined(SC_COMPLETE)
#define PRE_S ST_REPLY
#define PRE_B B_CONN
#define POST_S ST_READY
#define POST_B B_NULL
#elif defined(SC_RECV)
#define PRE_S ST_RECEIVE
#define PRE_B B_CHAN
#define POST_S ST_READY
#define POST_B B_THR
#elif defined(SC_SELF)
#define PRE_S ST_REPLY
#define PRE_B B_CONN
#define POST_S ST_READY
#define POST_B B_SELF
#elif defined(SC_RECVPUB)
#define PRE_S ST_RUN
#define PRE_B B_NULL
#define POST_S ST_RECEIVE
#define POST_B B_CHAN
#else
#error "define exactly one of SC_PUBLISH SC_PUBSEND SC_COMPLETE SC_RECV SC_SELF SC_RECVPUB"
#endif

/* The if-chain above silently picks the first match, so reject combinations. */
#if (defined(SC_PUBLISH) + defined(SC_PUBSEND) + defined(SC_COMPLETE) + defined(SC_RECV) + \
     defined(SC_SELF) + defined(SC_RECVPUB)) != 1
#error "define exactly one SC_* scenario"
#endif
#if defined(SERIAL_CTRL) && defined(WRITER_SPLIT)
#error "SERIAL_CTRL and WRITER_SPLIT are mutually exclusive"
#endif
#if defined(PROBE_PRE) && defined(PROBE_POST)
#error "PROBE_PRE and PROBE_POST are mutually exclusive"
#endif
#if (defined(PROBE_PRE) || defined(PROBE_POST)) && !defined(SERIAL_CTRL)
#error "PROBE_PRE/PROBE_POST are only meaningful with SERIAL_CTRL"
#endif
#if (defined(PROBE_PRE) || defined(PROBE_POST)) && defined(GUARD_NULL)
#error "PROBE_PRE/PROBE_POST cannot be combined with GUARD_NULL"
#endif
#if defined(GUARD_NULL) && (defined(SERIAL_CTRL) || defined(WRITER_SPLIT))
#error "GUARD_NULL is a separate control; do not combine with SERIAL_CTRL/WRITER_SPLIT"
#endif

byte state = PRE_S;
byte bo = PRE_B;

/*
 * The kernel's SHORT_MSG/BUFF_MSG flag stores sit next to the blocked_on stores
 * in SC_RECV and SC_SELF, but debug_thread()'s state/blocked_on switch never
 * reads them, so they are deliberately not modelled (they could not change any
 * outcome here).
 */
bool ser = false;
/* Distinct violation flags: the gate matches the assertion text by name. */
bool debug_null_deref = false;
bool debug_cross_type = false;
bool probe_pre = false;     /* SERIAL_CTRL probes: reader saw the exact pre-writer pair */
bool probe_post = false;    /* ... or the exact post-writer pair */

inline lk() { atomic { !ser -> ser = true } }
inline ul() { ser = false }

/* reader pair and whole-writer exclusion (SERIAL_CTRL); per-store (WRITER_SPLIT) */
#if defined(SERIAL_CTRL) || defined(WRITER_SPLIT)
inline rd_lk() { lk() }
inline rd_ul() { ul() }
#else
inline rd_lk() { skip }
inline rd_ul() { skip }
#endif
#ifdef SERIAL_CTRL
inline wa_lk() { lk() }
inline wa_ul() { ul() }
#else
inline wa_lk() { skip }
inline wa_ul() { skip }
#endif
#ifdef WRITER_SPLIT
inline ws_lk() { lk() }
inline ws_ul() { ul() }
#else
inline ws_lk() { skip }
inline ws_ul() { skip }
#endif

/* each store is its own step so WRITER_SPLIT can lock them individually */
#define ST(var, val) ws_lk(); var = val; ws_ul()

proctype writer()
{
    wa_lk();
#if defined(SC_PUBLISH)
    ST(state, ST_REPLY);    /* sender->state = STATE_REPLY, written directly */
    ST(bo, B_CONN);         /* sender->blocked_on = cop; no clear in between */
#elif defined(SC_PUBSEND)
    ST(state, ST_SEND);     /* unready(sender, STATE_SEND) */
    ST(bo, B_NULL);         /* block() clears blocked_on for STATE_SEND */
    ST(bo, B_CONN);         /* sender->blocked_on = cop, after unready() */
#elif defined(SC_COMPLETE)
    ST(bo, B_NULL);         /* reply / force_ready, under chp->lock */
    ST(state, ST_READY);    /* ready(thp) afterwards */
#elif defined(SC_RECV)
    ST(bo, B_THR);          /* thp->blocked_on = act */
    ST(state, ST_READY);    /* block_and_ready(thp) afterwards */
#elif defined(SC_SELF)
    ST(bo, B_SELF);         /* thp->blocked_on = thp */
    ST(state, ST_READY);    /* ready(thp) afterwards */
#elif defined(SC_RECVPUB)
    ST(state, ST_RECEIVE);  /* unready(act, STATE_RECEIVE) */
    ST(bo, B_NULL);         /* block() zeroes blocked_on */
    ST(bo, B_CHAN);         /* act->blocked_on = chp, restored afterwards */
#endif
    wa_ul();
}

proctype reader()
{
    byte s, b, want;

    rd_lk();
    s = state;              /* switch (dtp->state = thp->state) */
    b = bo;                 /* thp->blocked_on */
    rd_ul();
    printf("OBS s=%d b=%d\n", s, b);   /* shown on trail replay; the gate checks it */
#ifdef PROBE_PRE
    probe_pre = (s == PRE_S && b == PRE_B);
    assert(!probe_pre);
#endif
#ifdef PROBE_POST
    probe_post = (s == POST_S && b == POST_B);
    assert(!probe_post);
#endif

    if
    :: s == ST_SEND || s == ST_REPLY -> want = B_CONN
    :: s == ST_RECEIVE -> want = B_CHAN
    :: else -> want = B_ANY
    fi;
    if
    :: want == B_ANY -> skip
    :: want != B_ANY && b == B_NULL ->
#ifndef GUARD_NULL
        debug_null_deref = true
#else
        skip
#endif
    :: want != B_ANY && b != B_NULL && b != want -> debug_cross_type = true
    :: else -> skip
    fi;
    assert(!debug_null_deref);
    assert(!debug_cross_type)
}

init
{
    atomic {
        run writer();
        run reader()
    }
}
