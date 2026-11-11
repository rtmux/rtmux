/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Section 13 abstract-model companion: temporal liveness of the SRR rendezvous.
 *
 * Models the lost-wakeup hazard in kernel/ipc/fastmsg.c: a sender that finds no
 * receiver and a receiver that finds no sender must not both block forever with
 * a partner available.  The code probes the opposite queue, drops chp->lock,
 * and -- this is the fix -- re-probes it under chp->lock again before committing
 * to block (fastmsg.c re-probes chp->send_queue under chp->lock, ~line 1917);
 * the channel lock serializes the two queue insertions so whichever party blocks
 * second observes the first's queue entry and completes the rendezvous.  The
 * first `atomic` check below collapses the initial locked probe to an atomic
 * observation of the opposite queue; the modeled re-probe is the second
 * under-lock check, which is the one the lost-wakeup fix depends on.
 *
 * LTL `progress` (<> delivered) must hold.  Built with -DNO_REPROBE the re-probe
 * is dropped, both parties block, and progress is violated -- that is the
 * committed non-vacuity witness: the property is falsifiable, not vacuously true.
 */

bool send_q = false; /* a sender is queued on the channel */
bool recv_q = false; /* a receiver is queued on the channel */
bool lock = false;   /* the per-channel lock (chp->lock) */
bool delivered = false;

inline acquire() { atomic { !lock -> lock = true } }
inline release() { lock = false }

active proctype sender() {
    /* Lock-free fast check: a receiver already parked? */
    if
    :: atomic { recv_q -> recv_q = false; delivered = true }
    :: else ->
        acquire();
#ifndef NO_REPROBE
        /* Under-lock re-probe: a receiver may have queued in the window. */
        if
        :: recv_q -> recv_q = false; release(); delivered = true
        :: else ->
            send_q = true;
            release();
            (delivered) /* block on send_queue until a receiver takes us */
        fi
#else
        send_q = true;
        release();
        (delivered)
#endif
    fi
}

active proctype receiver() {
    if
    :: atomic { send_q -> send_q = false; delivered = true }
    :: else ->
        acquire();
#ifndef NO_REPROBE
        if
        :: send_q -> send_q = false; release(); delivered = true
        :: else ->
            recv_q = true;
            release();
            (delivered)
        fi
#else
        recv_q = true;
        release();
        (delivered)
#endif
    fi
}

ltl progress { <> delivered }
