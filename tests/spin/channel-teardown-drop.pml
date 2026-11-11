/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Channel teardown vs last-client-drop vs server-slot detach (one connect_t).
 *
 * Source (kernel/ipc/channel.c, connect-core.c, connect.c):
 *
 *   D = ksys_channel_destroy, "find all server connections" loop, one slot:
 *         read chancons[i] (no vec_slock); cop->channel == chp && scoid == i
 *         vector_rem(chancons, i)               (result ignored)
 *         if (cop->links == 0) connect_soul_free(cop)      plain load
 *         else cop->channel = NULL                         plain store
 *       then rcu_sched_synchronize(); object_free(chp).
 *   C = last client reference dropped: ksys_connect_detach -> connect_client_drop
 *         [connobj_rcu] old = atomic links--; if old == 1:
 *            ch = cop->channel (acquire)
 *            NULL        -> reap: caller connect_soul_free(cop)
 *            DISCONNECT  -> notification_deliver(ch); process = 0; NO free
 *            else        -> nothing (relies on channel_destroy / server detach)
 *   S = server thread ConnectDetach(scoid | SIDE_CHANNEL) in the same process:
 *         [vec_slock] lookup; links != 0 -> EINVAL; vector_rem; [unlock]
 *         if (cop->links == 0) connect_soul_free(cop)      (re-read, no lock)
 *
 * connect_soul_free = rcu_sched_synchronize + object_free.  Assumption A1 (not
 * proven here): rcu-sched waits for a hart that is inside a kernel call, so
 * D's per-iteration window (slot read .. cop->next read) counts as a reader,
 * and so does C's connobj_rcu section.  S's tail after vec_slock/rcu unlock
 * does NOT.  vector_rem is modelled as an ATOMIC check-and-clear; the real one
 * is a non-atomic freelist splice (see report), so the model is generous.
 *
 * Properties, checked on every path:
 *   - no cop access after free (UAF), chp not freed under a C reader
 *   - cop freed at most once (double free)
 *   - at the end the cop was freed, or is still published in chancons (so
 *     process-exit reap can find it); otherwise it is an ORPHAN (leak: slot
 *     unpublished, fd gone, channel gone, nobody will free it)
 *
 * Scenario macros (exactly the actors to run):
 *   -DWITH_C  client last drop, links starts 1       -DWITH_D  channel destroy
 *   -DWITH_S  server ConnectDetach of the scoid
 *   -DCLIENT_GONE  links starts 0 (client already dropped + notice sent)
 *   -DSERIAL_CTRL  D's whole iteration and C's whole drop are mutually
 *                  exclusive (non-vacuity control, NOT a proposed fix)
 *   -DSLOT_CTRL    D revalidates and claims the slot before reaping; an
 *                  abstract control for the D/S race, not a complete fix
 * The client/drop scenario excludes CHF_DISCONNECT and does not model notices,
 * reservation release, vector-slot reuse, or weak memory ordering. The terminal
 * property assumes a retained server slot can later be reaped on process exit;
 * that reaper and the channel-flags read are not modeled.
 */

#define NULLCH 0
#define CHP    1

#ifdef CLIENT_GONE
byte links = 0;
#else
byte links = 1;
#endif
byte channel = CHP;
bool slot = true;        /* chancons[scoid] -> cop */
byte freed = 0;          /* object_free count of cop */
bool rd_c = false, rd_d = false, rd_s = false;  /* rcu readers */
bool ser = false;
byte done = 0;

#ifdef SERIAL_CTRL
inline sl() { atomic { !ser -> ser = true } }
inline su() { ser = false }
#else
inline sl() { skip }
inline su() { skip }
#endif

#define TOUCH assert(freed == 0)   /* cop dereference */

/* connect_soul_free: synchronize waits for every OTHER reader, then frees. */
inline soul_free(me) {
    atomic { (me == 1 -> (!rd_d && !rd_s) : (me == 2 -> (!rd_c && !rd_s) : (!rd_c && !rd_d))) ->
        freed++ }
    assert(freed <= 1)
}

#ifdef WITH_C
active proctype client() {
    byte old, ch;
    bool reap = false;
    sl();
    rd_c = true;                          /* rcu_read_lock(connobj_rcu) */
    atomic { TOUCH; old = links; links = links - 1 }
    assert(old != 0);
    if
    :: old == 1 ->
        atomic { TOUCH; ch = channel }
        if
        :: ch == NULLCH -> reap = true
        :: ch == CHP -> skip
        fi
    :: else -> skip
    fi
    rd_c = false;
    su();
    if :: reap -> soul_free(1) :: else -> skip fi
    done++
}
#endif

#ifdef WITH_D
active proctype destroyer() {
    bool hit, won = true;
    byte l, ch;
    sl();
    rd_d = true;                          /* no quiescent state in iteration */
    atomic { hit = slot }                 /* VECP2(cop, &chancons, i) */
    if
    :: hit ->
        atomic { TOUCH; ch = channel }
        if
        :: ch == CHP ->
#ifdef SLOT_CTRL
            atomic { if :: slot -> slot = false :: else -> won = false fi }
#else
            atomic { slot = false }       /* vector_rem, result ignored */
#endif
            if
            :: won ->
            atomic { TOUCH; l = links }
            if
            :: l == 0 -> rd_d = false; soul_free(2)
            :: else ->
                atomic { TOUCH; channel = NULLCH }   /* cop->channel = NULL */
                atomic { TOUCH }                     /* cop = cop->next */
                rd_d = false
            fi
            :: else -> rd_d = false
            fi
        :: else -> rd_d = false
        fi
    :: else -> rd_d = false
    fi
    su();
    atomic { !rd_c -> skip }             /* rcu_sched_synchronize; free chp */
    done++
}
#endif

#ifdef WITH_S
active proctype srv_detach() {
    byte l;
    bool got = false;
    rd_s = true;                          /* connobj_rcu + vec_slock */
    atomic {
        if
        :: slot -> TOUCH; if :: links != 0 -> skip :: else -> slot = false; got = true fi
        :: else -> skip
        fi
    }
    rd_s = false;                         /* unlock + rcu_read_unlock */
    if
    :: got ->
        atomic { TOUCH; l = links }       /* "if (cop->links == 0)" re-read */
        if :: l == 0 -> soul_free(3) :: else -> skip fi
    :: else -> skip
    fi
    done++
}
#endif

#ifdef WITH_C
#define NC 1
#else
#define NC 0
#endif
#ifdef WITH_D
#define ND 1
#else
#define ND 0
#endif
#ifdef WITH_S
#define NS 1
#else
#define NS 0
#endif

init {
    (done == NC + ND + NS);
    /* terminal: freed once, still published in chancons (process-exit reap),
     * or still held by a client fd/sender (links != 0, later drop reaps) */
    assert(freed == 1 || slot || links != 0);
}
