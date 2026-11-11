/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Q76(a) reproduction: reply-timeout activation vs a concurrent force_ready.
 *
 * Source scenario (kernel/ipc/fastmsg.c, SEND->REPLY claim in ksys_msg_receivev,
 * commit 4965a01b): a SEND-blocked sender `thp` is claimed by the receiver's
 * hart under chp->lock (RTMUX_ITF_MSG_DELIVERY), linked on reply_queue in
 * STATE_REPLY, and then END_SMP_XFER drops the claim and chp->lock is released.
 * Only afterwards does the receiver run
 *     if (thp->timeout_flags & RTMUX_TIMEOUT_REPLY) timeout_start(thp);
 * timeout_start_impl (kernel/time/timer-core.c) is, on the sender's fields and
 * with no lock shared with force_ready:
 *     test  timeout_flags & (1 << state)         plain loads
 *     test  !(timeout_flags & TIMEOUT_ACTIVE)    plain load
 *     timeout_flags |= TIMEOUT_ACTIVE            plain load/or/store
 *     timer_activate_impl()                      takes timer_wheel_slock
 * With MSG_DELIVERY dropped, a signal/kill force_ready(thp) on another hart is
 * no longer deferred (force_ready_msgxfer).  It unlinks thp from reply_queue
 * under chp->lock, then ready() runs `if (timeout_flags) timeout_stop(thp)`:
 *     test  timeout_flags & TIMEOUT_ACTIVE
 *     timer_deactivate()  (wheel lock)
 *     timeout_flags = 0                          plain store
 * and finally sets STATE_READY.  Nothing orders the two sequences, so the stop
 * can run between the start's checks and its activation.  Outcomes:
 *   (1) stop sees ACTIVE clear, skips deactivate, zeroes flags; start then arms
 *       the timer on a thread that has already left the wait (orphan timer);
 *   (2) start sets ACTIVE, stop deactivates a timer not yet inserted (no-op),
 *       zeroes flags; start then inserts the timer (orphan timer);
 *   (3) stop's `flags = 0` lands between start's load and store of the |=,
 *       resurrecting TIMEOUT_ACTIVE on a READY thread.
 * An orphan timer later times out whatever the thread is waiting on next.
 *
 * Safety property (checked as an assertion once both sequences finished):
 * a thread whose reply wait was completed by force_ready owns no armed timer
 * and no TIMEOUT_ACTIVE bit.  The model is expected to VIOLATE it; the committed
 * check (scripts/check-reply-timeout-race.sh) asserts that, replays the trail,
 * and asserts that the -DSERIAL_CTRL control (start and stop made atomic with
 * respect to each other) does NOT violate it.
 *
 * What is NOT modelled: timeout_deferred_fire() (the past-deadline case) and
 * timer expiry itself; the WAIT_PENDING defer branch is not taken here because
 * the receiver has cleared the bit before this point. The model starts with a
 * REPLY-only timeout and no active timer; SEND|REPLY timers already armed in
 * STATE_SEND are outside this model. Whether any protocol
 * covers both activation and deferred delivery is left OPEN: SERIAL_CTRL is a
 * non-vacuity control, not a proposed fix, and no such lock exists in the tree.
 * The replier is not a second racer: it is the same hart and runs after
 * timeout_start in program order.
 */

#define S_REPLY 0
#define S_READY 1

#define F_REPLY  1  /* timeout_flags: REPLY bit (1 << STATE_REPLY) configured */
#define F_ACTIVE 2  /* RTMUX_FLG_TIMEOUT_ACTIVE */

byte state = S_REPLY;     /* thp->state */
byte flags = F_REPLY;     /* thp->timeout_flags (plain, unlocked) */
bool timer_armed = false; /* thp->timeout is on the wheel */
bool sync_ctl = false;    /* SERIAL_CTRL only: start/stop mutual exclusion */
byte done = 0;

#ifdef SERIAL_CTRL
inline ctl_lock()   { atomic { !sync_ctl -> sync_ctl = true } }
inline ctl_unlock() { sync_ctl = false }
#else
inline ctl_lock()   { skip }
inline ctl_unlock() { skip }
#endif

/* receiver hart: timeout_start(thp) after MSG_DELIVERY was dropped */
active proctype starter() {
    byte f, s, t;

    ctl_lock();
    f = flags;                         /* fastmsg.c: thp->timeout_flags & REPLY */
    if
    :: !(f & F_REPLY) -> goto out
    :: else
    fi;
    f = flags; s = state;              /* timeout_start_impl: flags & (1<<state) */
    if
    :: !((f & F_REPLY) && s == S_REPLY) -> goto out
    :: else
    fi;
    f = flags;                         /* !(flags & TIMEOUT_ACTIVE) */
    if
    :: (f & F_ACTIVE) -> goto out
    :: else
    fi;
    t = flags;                         /* flags |= TIMEOUT_ACTIVE: load ... */
    flags = t | F_ACTIVE;              /* ... store */
    atomic { timer_armed = true }      /* timer_activate_impl under wheel lock */
out:
    ctl_unlock();
    done++
}

/* signal/kill hart: force_ready(thp) on STATE_REPLY, claim already dropped */
active proctype forcer() {
    byte g, h;

#ifndef NO_FORCE
    /* Unlink under chp->lock; the starter does not acquire that lock. */

    ctl_lock();
    g = flags;                         /* ready(): if (timeout_flags) ... */
    if
    :: g != 0 ->
        h = flags;                     /* timeout_stop: flags & TIMEOUT_ACTIVE */
        if
        :: (h & F_ACTIVE) -> atomic { timer_armed = false }  /* timer_deactivate */
        :: else
        fi;
        flags = 0                      /* thp->timeout_flags = 0 */
    :: else
    fi;
    ctl_unlock();
    state = S_READY;
#endif
    done++
}

active proctype checker() {
    done == 2;
#ifdef NO_FORCE
    /* The no-racer control must still arm a timer, not merely avoid the race. */
    assert(timer_armed && (flags & F_ACTIVE));
#endif
    /* Reply wait completed by force_ready: no armed timer, no ACTIVE bit. */
    assert(state == S_REPLY || (!timer_armed && !(flags & F_ACTIVE)))
}
