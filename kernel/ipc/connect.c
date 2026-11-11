/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <user-slice.h>
#include <sys/procmsg.h>

/* Revoke every endpoint-cap-table slot naming "object", across every process
 * table, before the object's soul is recycled. A cap over an object can be
 * IPC-transferred to a process other than the owner (a server minting a cap on
 * a client's connection at message receipt, for example), so revoking only the
 * owner's table would leave a live cap that a later object_alloc could resolve
 * to a reused soul. Matches on the raw object pointer, so it serves any
 * cap-bearing object class (connections here, and process teardown reuses it).
 *
 * Pin one process at a time under process_vector_slock, then drop the lock
 * before scanning that process's cap table. rtmux_cap_revoke_object is O(nslots
 * squared) worst case (a process holding many caps over one object in distinct
 * revocation groups), and holding process_vector_slock, a kspinlock, across it
 * would put that scan in the interrupt-off window. The pin blocks the process
 * soul-free: kext_process_destroy does vector_rem then process_wait_unpinned
 * before object_free, so a pin taken while prp is still in the vector keeps
 * prp->endpoint_caps valid across the unlocked scan. process_vector removal
 * leaves a freelist-tagged hole that VECP treats as empty and never shifts
 * indices, so index iteration across the drop covers exactly the set a
 * single-lock walk would. Only the interrupt-off window shrinks, to the O(1)
 * pin.
 *
 * A non-EOK process_pin only skips a slot that cannot name a live object: a
 * dying process past pid clear (ESRCH), or a process still being created
 * (published into the vector at kext_process_create before its pid is assigned,
 * also ESRCH) whose endpoint table was just zeroed. EAGAIN needs UINT_MAX live
 * pins and is unreachable. Callers must guarantee no new cap over "object" can
 * be minted during the walk (the object is already unpublished).
 */
void cap_revoke_object_all_tables(const void *object)
{
    unsigned i;

    for (i = 1;; i++) {
        process_t *prp;

        kspinlock_lock(&process_vector_slock);
        if (i >= process_vector.nentries) {
            kspinlock_unlock(&process_vector_slock);
            break;
        }
        prp = VECP(prp, &process_vector, i);
        if (prp && process_pin(prp) != EOK)
            prp = NULL;
        kspinlock_unlock(&process_vector_slock);

        if (prp) {
            (void) rtmux_cap_revoke_object(&prp->endpoint_caps, object);
            process_unpin(prp);
        }
    }
}

/* Fan out a connection's endpoint-cap revocation before its soul frees. Guard
 * on cop alone, not cop->process: the DISCONNECT path in connect_detach nulls
 * cop->process while keeping the soul alive, and a later free still has to
 * revoke by object pointer. The normal teardown callers have already
 * unpublished cop; the connect_attach NETCON reject caller holds a fresh cop
 * with cap 0 under both participant vec_slocks, so it too is unobservable to a
 * minter. Declared in decls.h so connect_detach in connect-core.c can reach it.
 */
void endpoint_cap_revoke_if_present(connect_t *cop)
{
    if (!cop)
        return;
    cap_revoke_object_all_tables(cop);
    cop->cap = 0;
}

/* Release the notification soul reserved for cop's disconnect notice, if it
 * still holds one. Safe to repeat: nresv is cleared with the release, and every
 * caller owns cop exclusively (a fresh attach, the last-link detach, or the
 * final free), so the clear needs no lock.
 */
void connect_unreserve(connect_t *cop)
{
    if (cop && cop->nresv) {
        cop->nresv = 0;
        object_unreserve(&notification_souls);
    }
}

/* Recycle a connection soul that has been unpublished from every vector it was
 * reachable through. The three steps must always run together and in this
 * order, so route every post-publication free of a connect_t through here:
 *
 *   1. Drain RCU so any in-flight connobj_rcu reader still holding a stale cop
 *      pointer exits before the memory is reused (closes the cop->channel deref
 *      UAF a concurrent notification_deliver could otherwise hit).
 *   2. Revoke every endpoint capability naming this connection across all
 *      process tables (see endpoint_cap_revoke_if_present for why the fan-out
 *      covers the owner plus any transferee).
 *   3. Return the soul to the pool.
 *
 * Centralizing the sequence keeps a future teardown path from freeing a soul
 * while a transferred cap still names it. Only the connect_attach allocation
 * error paths free a fresh, never-published cop directly, and they need none of
 * these steps.
 */
void connect_soul_free(process_t *prp, connect_t *cop)
{
    connect_unreserve(cop);
    rcu_sched_synchronize();
    endpoint_cap_revoke_if_present(cop);
    object_free(prp, &connect_souls, cop);
}

static void endpoint_cap_revoke_coid(connect_t *cop, uint64_t coid)
{
    if (cop && cop->process)
        (void) rtmux_cap_revoke_object_context(&cop->process->endpoint_caps,
                                               cop, coid);
}

static int endpoint_cap_pin_context(process_t *prp,
                                    uint64_t handle,
                                    uint64_t *contextp,
                                    connect_t **copp,
                                    int *pinnedp);

static int endpoint_cap_pin_context(process_t *prp,
                                    uint64_t handle,
                                    uint64_t *contextp,
                                    connect_t **copp,
                                    int *pinnedp)
{
    void *object = NULL;
    uint64_t context = handle;
    int r;

    if (pinnedp)
        *pinnedp = 0;
    if (rtmux_cap_generation((rtmux_cap_t) handle) == 0) {
        if (contextp)
            *contextp = context;
        if (copp)
            *copp = NULL;
        return EOK;
    }

    r = rtmux_cap_lookup_pin_context(&prp->endpoint_caps, (rtmux_cap_t) handle,
                                     RTMUX_CAP_ENDPOINT_SEND, &object,
                                     &context);
    if (r != EOK)
        return r;
    if (contextp)
        *contextp = context;
    if (copp)
        *copp = object;
    if (pinnedp)
        *pinnedp = 1;
    return EOK;
}

/* Two-process vec_slock helpers. Acquire in pointer order to avoid AB/BA
 * deadlock between two simultaneous attaches with swapped client/server roles.
 * Same-class (PROCESS) distinct-instance acquires rely on the lock_debug
 * relaxation that permits them.
 */
static inline void vec_slocks_lock(process_t *a, process_t *b)
{
    if (a == b) {
        kspinlock_lock(&a->vec_slock);
    } else if ((uintptr_t) &a->vec_slock < (uintptr_t) &b->vec_slock) {
        kspinlock_lock(&a->vec_slock);
        kspinlock_lock(&b->vec_slock);
    } else {
        kspinlock_lock(&b->vec_slock);
        kspinlock_lock(&a->vec_slock);
    }
}

static inline void vec_slocks_unlock(process_t *a, process_t *b)
{
    if (a == b) {
        kspinlock_unlock(&a->vec_slock);
    } else if ((uintptr_t) &a->vec_slock < (uintptr_t) &b->vec_slock) {
        kspinlock_unlock(&b->vec_slock);
        kspinlock_unlock(&a->vec_slock);
    } else {
        kspinlock_unlock(&a->vec_slock);
        kspinlock_unlock(&b->vec_slock);
    }
}

/* Decrement a process's open-connection count under its vec_slock.
 *
 * nfds is bumped under the owning process's vec_slock at attach
 * (connect_attach), so it must be decremented under the same lock. A plain
 * non-atomic -- races a concurrent attach's ++ on another hart and loses an
 * update, leaving nfds off by one at teardown (the intermittent process.c:465
 * nfds != 0 crash, seen as either a leak or a wrap-to-65535 underflow depending
 * on which update is lost). Callers must not already hold prp->vec_slock.
 */
static inline void process_nfds_dec(process_t *prp)
{
    kspinlock_lock(&prp->vec_slock);

    /* nfds is uint16_t; a mis-categorized detach would wrap it to 65535 and
     * resurface as the same process.c teardown crash this accounting fixes.
     * Fail at the source instead of far downstream.
     */
    CRASHCHECK(prp->nfds == 0);
    prp->nfds--;
    kspinlock_unlock(&prp->vec_slock);
}

/* Attach a connection to a channel (connect_attach kcall).
 *
 * Resolves the target process and channel, reuses an existing shareable
 * connection where possible, otherwise allocates a new connect_t and publishes
 * it into the client's fd/side-channel vector and the server's channel vector.
 * Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (target pid/chid/nd, flags, requested index)
 * Return value: ENOERROR on success (coid in KSTATUS); an errno on failure.
 */
int kdecl ksys_connect_attach(thread_t *act,
                              struct kcall_args_connect_attach *kap)
{
    process_t *prpc, *prps;
    channel_t *chp = NULL;
    connect_t *cop, *cop_master = NULL;
    vector_t *vec = NULL;
    vector_t *newvec;
    int i, coid, scoid, special;
    unsigned chid = 0;
    unsigned status;
    int chid_is_global = 0;
    int prps_pinned = 0;

#define CONNECT_ATTACH_RETURN(rc) \
    do {                          \
        if (prps_pinned)          \
            process_unpin(prps);  \
        return (rc);              \
    } while (0)

    /* Locking participants:
     *   - prpc->vec_slock + prps->vec_slock via vec_slocks_lock()
     *     for fdcons/chancons mutations.
     *   - global_channel_slock for the global_channel_vector lookup on the
     *     RTMUX_GLOBAL_CHANNEL path; held only across the lookup,
     *     released before vec_slocks_lock so the rank ordering of
     *     two PROCVEC-class instances stays unambiguous.
     *   - cop_master->lock (CONNECT) for the VCONNECT chain
     *     insertion.
     *
     * Residual: the chp pointer captured here remains pointer-stable only while
     * it has at least one connection (or the destroy path hasn't begun). A
     * concurrent ksys_channel_destroy that runs after our lookup but before our
     * LINK1_BEG into chp->cons can null cop->channel for any existing cop and
     * free chp — the same RCU-retire window as lookup_rcvid.
     */

    prpc = act->process;

    /* The nfds/rlimit exemption is a privileged-system authority, not a
     * property of the literal roottask pid: a process holding
     * RTMUX_PRIV_CAP_SYSTEM (roottask, and any server the bootstrap manifest
     * grants it) may exceed RLIMIT_NOFILE for connections. Capability-derived,
     * so it survives a roottask replacement. A started child runs with
     * priv_caps 0 and stays bounded. A child is transiently granted every cap
     * while RTMUX_FLG_PROC_LOADING so its roottask loader thread can set it up,
     * so exclude that window to match the former roottask-only semantics.
     */
    special = (prpc->priv_caps & RTMUX_PRIV_CAP_SYSTEM) != 0 &&
              !(prpc->flags & RTMUX_FLG_PROC_LOADING) && !kap->flags;

    if ((uint32_t) kap->flags &
        ~(RTMUX_FLG_COF_CLOEXEC | RTMUX_FLG_COF_DEAD | RTMUX_FLG_COF_NOSHARE |
          RTMUX_FLG_COF_NETCON | RTMUX_FLG_COF_NONBLOCK | RTMUX_FLG_COF_ASYNC |
          RTMUX_FLG_COF_GLOBAL)) {
        /* invalid flags specified */
        CONNECT_ATTACH_RETURN(EINVAL);
    }

    /* Don't let them pick an index for side channels. They always get first
     * avail.
     */
    if (kap->index & RTMUX_SIDE_CHANNEL) {
        kap->index = RTMUX_SIDE_CHANNEL;
        /* Make sure they don't pick one beyond their limits. */
    } else if (kap->index >= prpc->rlimit_vals_soft[RLIMIT_NOFILE]) {
        CONNECT_ATTACH_RETURN(EINVAL);
    }

    /* Make sure they are allowed more connections. Note: use '>' instead of
     * '>=' to take into account the proc channel that is created for each
     * process. This allows a process to open RLIMIT_NOFILE connections, even
     * though the proc channel is accounted for in prpc->nfds
     */
    if (!special && prpc->nfds > prpc->rlimit_vals_soft[RLIMIT_NOFILE]) {
        CONNECT_ATTACH_RETURN(EMFILE);
    }
    /* Check for a remote node. */
    if (RTMUX_NODE_CMP(kap->nd, RTMUX_LOCAL_NODE) != 0) {
        if ((prps = remote_ipc.prp) == 0 || (chp = remote_ipc.chp) == 0) {
            CONNECT_ATTACH_RETURN(EREMOTE);
        }
        goto common;
    }
    if (kap->pid == prpc->pid) {
        prps = prpc;
    } else if (kap->pid) {
        prps = lookup_pid_rcu(kap->pid);
        if (prps && process_pin(prps) == EOK) {
            prps_pinned = 1;
        } else {
            prps = NULL;
        }
        rcu_read_unlock(&process_rcu);
    } else {
        prps = prpc;
    }
    /* Make sure process is valid. */
    if (!prps) {
        CONNECT_ATTACH_RETURN(ESRCH);
    }

    kap->pid = prps->pid;

    vec = &prps->chancons;
    chid = kap->chid;
    chid_is_global = (chid & RTMUX_GLOBAL_CHANNEL) != 0;
    if (chid_is_global) {
        vec = &global_channel_vector;
        chid &= ~RTMUX_GLOBAL_CHANNEL;

        /* Global path: global_channel_slock around the global_channel_vector
         * lookup. Per-process path: lookup is deferred to inside the
         * vec_slocks_lock window further down so the lookup + vector_add are
         * atomic against destroy.
         */
        kspinlock_lock(&global_channel_slock);
        chp = vector_lookup(vec, chid);
        if (!chp || chp->type != TYPE_CHANNEL) {
            kspinlock_unlock(&global_channel_slock);
            CONNECT_ATTACH_RETURN(ESRCH);
        }

        /* Capability gate: a non-public global channel (required_cap != 0)
         * admits a connect only if the caller holds a live cap naming this
         * channel object, delegated by the registering server. Public channels
         * (required_cap == 0) stay open by chid. Object-identity match, not cap
         * id: an IPC-transferred cap has a receiver-local id but the same
         * object. Connect-time only, off the message hot path. The scan is
         * bounded by the fixed per-process endpoint-cap slot count, so holding
         * global_channel_slock across it adds a bounded, connect-only cost.
         */
        if (((global_channel_t *) chp)->required_cap != 0 &&
            !rtmux_cap_has_object(&prpc->endpoint_caps, chp,
                                  RTMUX_CAP_ENDPOINT_SEND)) {
            kspinlock_unlock(&global_channel_slock);
            CONNECT_ATTACH_RETURN(EPERM);
        }
        kspinlock_unlock(&global_channel_slock);
    }

    /* On the per-process path chp stays NULL here and is resolved below under
     * vec_slocks_lock. On the remote IPC goto-common path chp was already set
     * to remote_ipc.chp before the goto.
     */

common:
    if (prps->flags & (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_ZOMBIE |
                       RTMUX_FLG_PROC_COREDUMP)) {
        CONNECT_ATTACH_RETURN(ENXIO);
    }

    newvec =
        (kap->index & RTMUX_SIDE_CHANNEL) ? &prpc->chancons : &prpc->fdcons;

    /* Per-process vec_slock(s) for the entire vec-touching region: the reuse
     * search, the new-cop publication into prpc, and the server-side vector_add
     * into prps->chancons. Acquired in pointer order so two concurrent
     * connect_attach calls with swapped client/server roles can't deadlock. The
     * wide critical section is what makes the search-and-publish atomic.
     */
    vec_slocks_lock(prpc, prps);

    /* Per-process chp lookup happens inside the vec_slocks_lock window so it's
     * atomic with the subsequent search/publish steps. Skip when we already
     * resolved chp via the global_channel_slock path above, or via the remote
     * IPC goto-common path (where chp = remote_ipc.chp).
     */
    if (!chp) {
        chp = vector_lookup(vec, chid);
        if (!chp || chp->type != TYPE_CHANNEL) {
            vec_slocks_unlock(prpc, prps);
            CONNECT_ATTACH_RETURN(ESRCH);
        }
    }

    /* The remote IPC service uses this to prevent connection sharing. */
    if (((uint32_t) kap->flags &
         (RTMUX_FLG_COF_NOSHARE | RTMUX_FLG_COF_NETCON)) == 0) {
        /* Scan for an existing connection in either channel vector. */
        for (vec = &prpc->fdcons;;) {
            for (i = 0; i < vec->nentries; ++i) {
                cop = VECP2(cop, vec, i);
                if (cop && cop->type == TYPE_CONNECTION &&
                    cop->process == prpc && cop->channel && cop->links &&
                    (cop->flags & COF_NETCON) == 0 &&
                    cop->un.lcl.pid == kap->pid &&
                    cop->un.lcl.chid == kap->chid &&
                    RTMUX_NODE_CMP(cop->un.lcl.nd, kap->nd) == 0) {
                    if (cop->un.lcl.nd != kap->nd) {
                        /* same connection, different quality of service */
                        if (!cop_master) {
                            cop_master = (cop->flags & COF_VCONNECT)
                                             ? cop->un.lcl.cop
                                             : cop;
                        }
                    } else {
                        if ((coid = vector_add(
                                 newvec, cop,
                                 kap->index & ~RTMUX_SIDE_CHANNEL)) == -1) {
                            vec_slocks_unlock(prpc, prps);
                            CONNECT_ATTACH_RETURN(EAGAIN);
                        }
                        /* Check coid is allowed */
                        if ((kap->index & RTMUX_SIDE_CHANNEL) == 0 &&
                            !special &&
                            (unsigned) coid >=
                                prpc->rlimit_vals_soft[RLIMIT_NOFILE]) {
                            vector_rem(newvec, (unsigned) coid);
                            vec_slocks_unlock(prpc, prps);
                            CONNECT_ATTACH_RETURN(EMFILE);
                        }

                        /* cop is an existing connection — refcount bump is
                         * atomic. We've just vector_add'd a new client-side
                         * reference to cop above, which makes cop reachable
                         * through the new slot; the atomic increment makes the
                         * bump visible across CPUs. No cop->lock needed for the
                         * refcount itself.
                         */
                        (void) atomic_add_value(&cop->links, 1);

                        if ((uint32_t) kap->flags & RTMUX_FLG_COF_CLOEXEC) {
                            vector_flag(newvec, (unsigned) coid, 1);
                        }

                        status =
                            (unsigned) coid | (kap->index & RTMUX_SIDE_CHANNEL);
                        SETKSTATUS(act, status);
                        prpc->nfds++;

                        vec_slocks_unlock(prpc, prps);
                        CONNECT_ATTACH_RETURN(ENOERROR);
                    }
                }
            }

            if (vec == &prpc->chancons) {
                break;
            }

            vec = &prpc->chancons;
        }
    }

    /* Allocate a new conection. */
    if (!(cop = object_alloc(prpc, &connect_souls))) {
        vec_slocks_unlock(prpc, prps);
        CONNECT_ATTACH_RETURN(ENFILE);
    }

    /* Per-connection lock — initialized before vector_add publishes "cop" to
     * other CPUs, so any observer that finds it via vector_lookup sees a usable
     * lock. See objects.h for ordering.
     */
    kspinlock_init(&cop->lock, LOCK_CLASS_CONNECT, "connect_t.lock");
    cop->cap = 0;

    /* Add connect to the clients channel vector. */
    if ((coid = vector_add(newvec, cop, kap->index & ~RTMUX_SIDE_CHANNEL)) ==
        -1) {
        object_free(prpc, &connect_souls, cop);
        vec_slocks_unlock(prpc, prps);
        CONNECT_ATTACH_RETURN(EAGAIN);
    }
    /* Check coid is allowed */
    if ((kap->index & RTMUX_SIDE_CHANNEL) == 0 && !special &&
        (unsigned) coid >= prpc->rlimit_vals_soft[RLIMIT_NOFILE]) {
        vector_rem(newvec, (unsigned) coid);
        object_free(prpc, &connect_souls, cop);
        vec_slocks_unlock(prpc, prps);
        CONNECT_ATTACH_RETURN(EMFILE);
    }
    /* Add connect to the servers channel vector. */
    scoid = 0;
    cop->nresv = 0;
    if (!(chp->flags & RTMUX_FLG_CHF_GLOBAL)) {
        if (cop_master) {
            /* same connection, different quality of service */
            scoid = cop_master->scoid;

            /* cop_master refcount bump is atomic — same reasoning as the reuse
             * path above. Chain insertion and cop's VCONNECT flag set need
             * cop_master->lock to serialize the LINK1_BEG against a concurrent
             * ksys_connect_detach walking the next chain. cop itself is
             * published (via vector_add at line 147) but its next/flags writes
             * here race only against this same chain walk — cop_master's lock
             * covers the chain write end-to-end.
             */
            (void) atomic_add_value(&cop_master->links, 1);
            kspinlock_lock(&cop_master->lock);
            cop->flags |= COF_VCONNECT;
            cop->un.lcl.cop = cop_master;
            LINK1_BEG(cop_master->next, cop, connect_t);
            kspinlock_unlock(&cop_master->lock);
        } else {
            /* Reserve the soul connect_detach will need to tell the server
             * this client is gone, so the notice cannot be lost to an
             * allocation failure at teardown. Fail the attach instead.
             */
            if ((chp->flags & RTMUX_FLG_CHF_DISCONNECT) != 0) {
                if (!object_reserve(&notification_souls)) {
                    vector_rem(newvec, (unsigned) coid);
                    object_free(prpc, &connect_souls, cop);
                    vec_slocks_unlock(prpc, prps);
                    CONNECT_ATTACH_RETURN(EAGAIN);
                }
                cop->nresv = 1;
            }
            if ((scoid = vector_add(&prps->chancons, cop, 1)) == -1) {
                connect_unreserve(cop);
                vector_rem(newvec, (unsigned) coid);
                object_free(prpc, &connect_souls, cop);
                vec_slocks_unlock(prpc, prps);
                CONNECT_ATTACH_RETURN(EAGAIN);
            }
            cop->next = NULL;
        }
    } else {
        /* link the connection to the global channel queue */
        LINK1_BEG(((global_channel_t *) chp)->cons, cop, connect_t);
        cop->flags |= RTMUX_FLG_COF_GLOBAL;
    }

    cop->type = TYPE_CONNECTION;
    cop->process = prpc;
    cop->links = 1;
    cop->channel = chp;
    __atomic_store_n(&cop->rights, RTMUX_CAP_ENDPOINT_ALL, __ATOMIC_RELAXED);
    cop->scoid = (uint16_t) scoid;
    cop->infoscoid = -1;
    if (RTMUX_NODE_CMP(kap->nd, RTMUX_LOCAL_NODE) == 0) {
        /* In the remote case we don't know this information at this time. */
        cop->infoscoid = (int16_t) scoid;
    }
    /* Remote IPC service allocates a place to hold remote process info. */
    if ((uint32_t) kap->flags & RTMUX_FLG_COF_NETCON) {
        if (prpc != remote_ipc.prp || (kap->index & RTMUX_SIDE_CHANNEL) == 0) {
            endpoint_cap_revoke_if_present(cop);
            connect_unreserve(cop);
            vector_rem(&prps->chancons, (unsigned) scoid);
            vector_rem(newvec, (unsigned) coid);
            object_free(prpc, &connect_souls, cop);
            vec_slocks_unlock(prpc, prps);
            CONNECT_ATTACH_RETURN(EINVAL);
        }
        cop->flags |= COF_NETCON;
        cop->un.remote.coid = (uint32_t) coid;
        /* Remote credential fields are supplied by the remote IPC path. */
    } else {
        cop->un.lcl.nd = kap->nd;
        cop->un.lcl.pid = kap->pid;
        cop->un.lcl.chid = kap->chid;
        cop->un.lcl.coid = (uint32_t) coid;
    }

    if ((uint32_t) kap->flags & RTMUX_FLG_COF_CLOEXEC) {
        vector_flag(newvec, (unsigned) coid, 1);
    }

    status = (unsigned) coid | (kap->index & RTMUX_SIDE_CHANNEL);
    SETKSTATUS(act, status);

    prpc->nfds++;

    vec_slocks_unlock(prpc, prps);
    CONNECT_ATTACH_RETURN(ENOERROR);
#undef CONNECT_ATTACH_RETURN
}


/* _connect_detach — internal variant with explicit process pointer. Used during
 * process teardown when all threads are already dead. Called under the kernel
 * lock (via syscall_priv).
 */
int _connect_detach(process_t *prp, int coid_arg)
{
    connect_t *cop;
    vector_t *vec;
    unsigned coid;
    int server_side;

    vec = ((unsigned) coid_arg & RTMUX_SIDE_CHANNEL) ? &prp->chancons
                                                     : &prp->fdcons;
    coid = (unsigned) coid_arg & ~RTMUX_SIDE_CHANNEL;
    if (!(cop = vector_lookup2(vec, coid)) || cop->type != TYPE_CONNECTION)
        return EINVAL;
    server_side = vec == &prp->chancons &&
                  !(cop->flags & RTMUX_FLG_COF_GLOBAL) && cop->channel &&
                  cop->channel->process == prp && coid == cop->scoid;

    vector_rem(vec, coid);
    if (cop->links == 0) {
        connect_soul_free(prp, cop);
    } else {
        if (!(cop->flags & RTMUX_FLG_COF_GLOBAL)) {
            if (connect_links_drop(cop))
                connect_detach(cop, prp->terming_priority);
        } else {
            if (cop->channel &&
                (((global_channel_t *) cop->channel)->ev_coid == coid_arg))
                ((global_channel_t *) cop->channel)->ev_prp = NULL;
            if (connect_links_drop(cop)) {
                if (cop->channel)
                    LINK1_REM(((global_channel_t *) cop->channel)->cons, cop,
                              connect_t);
                connect_soul_free(cop->process, cop);
            }
        }
    }
    if (!server_side)
        process_nfds_dec(prp);

    return EOK;
}

int kdecl ksys_connect_detach(thread_t *act,
                              struct kcall_args_connect_detach *kap)
{
    process_t *prp = act->process;
    connect_t *cop;
    connect_t *cap_cop;
    channel_t *chp;
    vector_t *vec;
    uint64_t coid_context;
    uint64_t handle;
    unsigned coid;
    int cap_pinned = 0;
    int r;


    /* Make sure the connection is valid. */
    handle = kap->coid;
    rcu_read_lock(&connobj_rcu);
    r = endpoint_cap_pin_context(prp, handle, &coid_context, &cap_cop,
                                 &cap_pinned);
    if (r != EOK) {
        rcu_read_unlock(&connobj_rcu);
        return EINVAL;
    }
    kap->coid = coid_context;
    vec = (coid_context & RTMUX_SIDE_CHANNEL) ? &prp->chancons : &prp->fdcons;
    coid = (unsigned) coid_context & ~RTMUX_SIDE_CHANNEL;

    /* Validate and remove under the same vec_slock window so only one detach
     * can claim this slot. Once vector_rem() succeeds, later detach/lookup
     * users cannot find this client-side reference.
     */
    kspinlock_lock(&prp->vec_slock);
    if (!(cop = vector_lookup2(vec, coid)) || cop->type != TYPE_CONNECTION ||
        (cap_cop && cap_cop != cop) ||
        ((chp = cop->channel) && chp->process == prp &&
         !(cop->flags & RTMUX_FLG_COF_GLOBAL) &&
         kap->coid == (cop->scoid | RTMUX_SIDE_CHANNEL) && cop->links != 0)) {
        kspinlock_unlock(&prp->vec_slock);
        if (cap_pinned)
            (void) rtmux_cap_unpin(&prp->endpoint_caps, (rtmux_cap_t) handle);
        rcu_read_unlock(&connobj_rcu);
        return EINVAL;
    }
    vector_rem(vec, (unsigned) coid);
    kspinlock_unlock(&prp->vec_slock);
    if (cap_pinned)
        (void) rtmux_cap_unpin(&prp->endpoint_caps, (rtmux_cap_t) handle);
    rcu_read_unlock(&connobj_rcu);



    /* unblock any other threads that are blocked on the same connection with
     * EBADF we don't need to check for scoid because scoid can not be in
     * send/reply queues. COF_NETCON connections store the local coid in
     * args.ms.coid.
     */
    if (prp->num_active_threads > 1) {
        int tid;

        for (tid = 0; tid < prp->threads.nentries; tid++) {
            thread_t *thp;
            connect_t *bcop;

            /* Snapshot thp->blocked_on once and NULL-guard the dereferences
             * below. thp can transition out of STATE_SEND/REPLY/RECEIVE between
             * our state check and the blocked_on deref, which would otherwise
             * fault on a NULL blocked_on. Guarding the deref skips threads in
             * transitional states; force_ready'ing them isn't necessary anyway
             * (they'll see EBADF on their next kcall via the channel teardown
             * path).
             */
            if (!(thp = VECP(thp, &prp->threads, tid))) {
                KSYS_PREEMPT(act, ENOERROR);
                continue;
            }
            bcop = thp->blocked_on;
            if (!bcop) {
                KSYS_PREEMPT(act, ENOERROR);
                continue;
            }
            if (((thp->args.ms.coid == kap->coid) &&
                 (thp->state == STATE_SEND || (thp->state == STATE_REPLY &&
                                               !(bcop->flags & COF_NETCON)))) ||
                (thp->state == STATE_RECEIVE && thp->args.ri.id == kap->coid &&
                 bcop->type == TYPE_CONNECTION &&
                 (bcop->flags & RTMUX_FLG_COF_GLOBAL))) {
                force_ready(thp, EBADF);
            }
            KSYS_PREEMPT(act, ENOERROR);
        }
    }
    if (cop->flags & COF_NETCON) {
        int i;
        unsigned prio;
        thread_t *thp;

        for (i = 0; i < vthread_vector.nentries; ++i) {
            vthread_t *vthp;
            connect_t *cop1;

            KSYS_PREEMPT(act, ENOERROR);

            vthp = VECP(vthp, &vthread_vector, i);
            if (!vthp)
                continue;
            if (vthp->args.ms.coid != kap->coid)
                continue;
            switch (vthp->state) {
            case STATE_SEND:
            case STATE_REPLY:
                break;
            default:
                continue;
            }
            cop1 = vthp->blocked_on;
            if (cop1 != cop) {
                if (!(cop1->flags & COF_VCONNECT))
                    continue;
                if (cop1->un.lcl.cop != cop)
                    continue;
            }
            vector_rem(&vthread_vector, (unsigned) i);
            if (force_ready((thread_t *) (void *) vthp, EBADF)) {
                object_free(NULL, &vthread_souls, vthp);
            }
        }

        /* Connect detach notification for the remote IPC service. */
        if ((act->process == remote_ipc.prp) &&
            ((cop->channel && cop->channel->process != remote_ipc.prp) ||
             (cop->scoid != coid))) {
            prio = act->priority;
            if (cop->channel) {
                for (i = 0; i < cop->channel->process->threads.nentries; ++i) {
                    thp = VECP(thp, &cop->channel->process->threads, i);
                    if ((thp) && (thp->state == STATE_REPLY) &&
                        (thp->blocked_on == cop)) {
                        if (prio > thp->priority) {
                            prio = thp->priority;
                        }
                        force_ready(thp, ESRCH | _FORCE_NO_UNBLOCK);
                    }
                }
            }
            if (remote_ipc.chp) {
                notification_deliver(remote_ipc.chp, prio,
                                     _NOTIFICATION_CODE_NET_DETACH, kap->coid,
                                     0, 0);
            }
        }
    }

    if (cap_pinned)
        (void) rtmux_cap_revoke(&prp->endpoint_caps, (rtmux_cap_t) handle,
                                NULL);
    else
        endpoint_cap_revoke_coid(cop, kap->coid);

    if (cop->links == 0) {
        /* Server connect, unpublished from prp's vec above; just recycle it. */
        connect_soul_free(prp, cop);
    } else {
        /* Drop the fd's reference; the last drop sends the disconnect notice
         * (connect_client_drop) or, when the channel is already gone, frees the
         * soul. In-flight sends hold their own references (ksys_msg_sendv).
         */
        if (!(cop->flags &
              RTMUX_FLG_COF_GLOBAL)) { /* no scoid and disconnect notification
                                        for global channel connections
                                        */
            if (connect_client_drop(cop, act->real_priority))
                connect_soul_free(prp, cop);
        } else {
            if (cop->channel &&
                (((global_channel_t *) cop->channel)->ev_coid == kap->coid)) {
                /* get rid of any event registered */
                ((global_channel_t *) cop->channel)->ev_prp = NULL;
            }
            /* Global channels carry no sends (ksys_msg_sendv: ENOSYS), so
             * nothing can race this drop and the last one frees the soul, as
             * the process-exit path (_connect_detach) does. The old test was on
             * the OLD value being 0, which never held: every closed
             * global-channel connection lived until its process exited.
             */
            if (connect_links_drop(cop)) {
                if (cop->channel) {
                    LINK1_REM(((global_channel_t *) cop->channel)->cons, cop,
                              connect_t);
                }
                connect_soul_free(cop->process, cop);
            }
        }

        /* vec_slock was released earlier in this call (before the force_ready
         * sweep), so process_nfds_dec here is a fresh, non-nested acquire.
         */
        process_nfds_dec(act->process);
    }

    SETKSTATUS(act, 0);

    return ENOERROR;
}



/* Report information about the next server connection (connect_server_info
 * kcall).
 *
 * Searches a process's fd then side-channel vectors from the given coid for a
 * connection, optionally filling a _server_info, and returns the coid it found.
 * Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (target pid, starting coid, optional info buffer)
 * Return value: ENOERROR on success (coid in KSTATUS); an errno on failure.
 */
int kdecl ksys_connect_server_info(thread_t *act,
                                   struct kcall_args_connect_server_info *kap)
{
    process_t *prp;
    vector_t *vec;
    connect_t *cop;
    connect_t *cap_cop;
    channel_t *chp;
    unsigned coid;
    rtmux_cap_t endpoint_cap;
    unsigned flags;
    struct _server_info info;
    bool prp_rcu; /* whether we hold a process_rcu section on prp */
    bool exact_context;
    bool exact_side_channel;
    uint64_t coid_context;
    int cap_pinned = 0;

    if (kap->pid == act->process->pid) {
        prp = act->process;
        prp_rcu = false;
    } else if (kap->pid) {
        if (!(act->process->priv_caps & RTMUX_PRIV_CAP_PROCESS))
            return EPERM;
        prp = lookup_pid_rcu(kap->pid);
        prp_rcu = true;
        if (!prp) {
            rcu_read_unlock(&process_rcu);
            return EPERM;
        }
    } else {
        prp = act->process;
        prp_rcu = false;
    }
    exact_context = rtmux_cap_generation((rtmux_cap_t) kap->coid) != 0;
    rcu_read_lock(&connobj_rcu);
    if (endpoint_cap_pin_context(prp, kap->coid, &coid_context, &cap_cop,
                                 &cap_pinned) != EOK) {
        rcu_read_unlock(&connobj_rcu);
        if (prp_rcu)
            rcu_read_unlock(&process_rcu);
        return EBADF;
    }
    exact_side_channel = (coid_context & RTMUX_SIDE_CHANNEL) != 0;

    /* Hold prp->vec_slock for the search + flag/cop reads. Releases before the
     * user copy-out — once we've captured all _server_info scalar fields
     * locally, the vec ops are done. After release, the user copy may fault —
     * user_slice_put does that outside the lock.
     */
    kspinlock_lock(&prp->vec_slock);

    /* Get correct connection vector. */
    vec = (coid_context & RTMUX_SIDE_CHANNEL) ? &prp->chancons : &prp->fdcons;
    coid = (unsigned) coid_context & ~RTMUX_SIDE_CHANNEL;

    if (exact_context) {
        cop = vector_lookup2(vec, coid);
        if (!cop || cap_cop != cop || cop->type != TYPE_CONNECTION) {
            kspinlock_unlock(&prp->vec_slock);
            if (cap_pinned)
                (void) rtmux_cap_unpin(&prp->endpoint_caps,
                                       (rtmux_cap_t) kap->coid);
            rcu_read_unlock(&connobj_rcu);
            if (prp_rcu)
                rcu_read_unlock(&process_rcu);
            return EBADF;
        }
        chp = cop->channel;
        if ((chp && chp->process == prp && coid == cop->scoid &&
             vec == &prp->chancons) ||
            (!chp && !kap->info)) {
            kspinlock_unlock(&prp->vec_slock);
            if (cap_pinned)
                (void) rtmux_cap_unpin(&prp->endpoint_caps,
                                       (rtmux_cap_t) kap->coid);
            rcu_read_unlock(&connobj_rcu);
            if (prp_rcu)
                rcu_read_unlock(&process_rcu);
            return EBADF;
        }
        goto found;
    }

    /* Search for next connection (making sure to avoid server connections). */
    chp = NULL;
    for (;;) {
        while ((cop = vector_search(vec, coid, &coid))) {
            if (cop->type == TYPE_CONNECTION &&
                !((chp = cop->channel) && chp->process == prp &&
                  coid == cop->scoid && vec == &prp->chancons)) {
                /* Only report connection if channel is valid or info is
                 * requested
                 */
                if (chp || kap->info) {
                    break;
                }
            }
            coid++;
        }

        if (cop) {
            if (exact_context &&
                (coid != (((unsigned) coid_context) & ~RTMUX_SIDE_CHANNEL) ||
                 exact_side_channel != (vec == &prp->chancons) ||
                 (cap_cop && cap_cop != cop))) {
                kspinlock_unlock(&prp->vec_slock);
                if (cap_pinned)
                    (void) rtmux_cap_unpin(&prp->endpoint_caps,
                                           (rtmux_cap_t) kap->coid);
                rcu_read_unlock(&connobj_rcu);
                if (prp_rcu)
                    rcu_read_unlock(&process_rcu);
                return EBADF;
            }
            break;
        }

        if (vec == &prp->fdcons) {
            if (exact_context) {
                kspinlock_unlock(&prp->vec_slock);
                if (cap_pinned)
                    (void) rtmux_cap_unpin(&prp->endpoint_caps,
                                           (rtmux_cap_t) kap->coid);
                rcu_read_unlock(&connobj_rcu);
                if (prp_rcu)
                    rcu_read_unlock(&process_rcu);
                return EBADF;
            }
            vec = &prp->chancons;
            coid = 0;
        } else {
            /* No more connections found */
            kspinlock_unlock(&prp->vec_slock);
            if (cap_pinned)
                (void) rtmux_cap_unpin(&prp->endpoint_caps,
                                       (rtmux_cap_t) kap->coid);
            rcu_read_unlock(&connobj_rcu);
            if (prp_rcu)
                rcu_read_unlock(&process_rcu);
            return EINVAL;
        }
    }

found:
    /* save flags for later */
    flags = VECAND(VEC(vec, coid), 2) ? RTMUX_FLG_COF_CLOEXEC : 0;
    if (!chp) {
        flags |= RTMUX_FLG_COF_DEAD;
    }
    /* Was it from the side channel vector? */
    if (vec == &prp->chancons) {
        coid |= RTMUX_SIDE_CHANNEL;
    }
    endpoint_cap = 0;
    if (kap->info) {
        if (rtmux_cap_get_or_alloc_context(&prp->endpoint_caps, cop,
                                           (uint64_t) coid, cop->rights,
                                           &endpoint_cap) != EOK) {
            kspinlock_unlock(&prp->vec_slock);
            if (cap_pinned)
                (void) rtmux_cap_unpin(&prp->endpoint_caps,
                                       (rtmux_cap_t) kap->coid);
            rcu_read_unlock(&connobj_rcu);
            if (prp_rcu)
                rcu_read_unlock(&process_rcu);
            return ENOSPC;
        }
        if (cop->cap == 0)
            cop->cap = endpoint_cap;
    }
    if (kap->info) {
        memset(&info, 0, sizeof(info));
        if (cop->flags & COF_NETCON) {
            info.nd = cop->un.remote.nd;
            info.pid = cop->un.remote.pid;
            info.chid = -1;
        } else {
            info.nd = cop->un.lcl.nd;
            info.pid = cop->un.lcl.pid;
            info.chid = (int32_t) cop->un.lcl.chid;
        }
        if (cop->flags & COF_VCONNECT) {
            info.scoid = (int32_t) ((unsigned) cop->un.lcl.cop->infoscoid |
                                    RTMUX_SIDE_CHANNEL);
        } else {
            info.scoid =
                (int32_t) ((unsigned) cop->infoscoid | RTMUX_SIDE_CHANNEL);
        }
        info.coid = (int32_t) coid;
        info.flags = (uint16_t) flags;
        info.endpoint_cap = endpoint_cap;
        info.endpoint_rights = cop->rights;
    }

    kspinlock_unlock(&prp->vec_slock);
    if (cap_pinned)
        (void) rtmux_cap_unpin(&prp->endpoint_caps, (rtmux_cap_t) kap->coid);
    rcu_read_unlock(&connobj_rcu);
    if (prp_rcu)
        rcu_read_unlock(&process_rcu);

    if (kap->info && user_slice_put(act, kap->info, &info, sizeof(info)))
        return EFAULT;

    SETKSTATUS(act, coid);
    return ENOERROR;
}



/* Report credential/identity information about a connection's client
 * (connect_client_info kcall).
 *
 * With scoid == -1 the caller's own process is described; otherwise the
 * side-channel connection identified by scoid is looked up and its client
 * process is described. Fills the caller's _client_info including the
 * credential block (clamped to the requested ngroups). Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (scoid, info buffer, requested ngroups)
 * Return value: EOK on success; an errno on failure.
 */
int kdecl ksys_connect_client_info(thread_t *act,
                                   struct kcall_args_connect_client_info *kap)
{
    process_t *prp = act->process;
    connect_t *cop;
    channel_t *chp;
    client_info_t ci;
    struct user_slice ci_slice;
    unsigned scoid = (unsigned) kap->scoid & ~RTMUX_SIDE_CHANNEL;


    /* If scoid is -1 it get info on calling process */
    cop = NULL;
    if (kap->scoid != -1) {
        /* vector_lookup on prp->chancons under prp->vec_slock so the cop
         * pointer can't be vector_rem'd between lookup and the subsequent field
         * reads. Released as soon as cop is captured.
         */
        kspinlock_lock(&prp->vec_slock);
        if (((unsigned) kap->scoid & RTMUX_SIDE_CHANNEL) == 0 ||
            !(cop = vector_lookup(&prp->chancons, scoid)) ||
            (cop->type != TYPE_CONNECTION) || !(chp = cop->channel) ||
            chp->process != prp || cop->scoid != scoid) {
            kspinlock_unlock(&prp->vec_slock);
            return EINVAL;
        }
        kspinlock_unlock(&prp->vec_slock);

        prp = cop->process;
    }

    if (kap->info) {
        client_info_t *clp = &ci;
        struct _cred_info *cip;
        unsigned ngroups;

        if (user_slice_init(act, kap->info, sizeof(ci), sizeof(ci),
                            USER_SLICE_WR, &ci_slice))
            return EFAULT;
        memset(clp, 0, sizeof(*clp));

        if (cop && (cop->flags & COF_NETCON) != 0) {
            credential_t *crp;

            if (!(crp = cop->un.remote.cred)) {
                /* Not a complete connection yet!! */
                return EINVAL;
            }
            cip = &crp->info;
            clp->nd = cop->un.remote.nd;
            clp->pid = cop->un.remote.pid;
            clp->sid = 0; /* sid dropped — see comment below */
            clp->flags = cop->un.remote.flags;
        } else {
            /* RTMux no longer bundles POSIX session info into _client_info: the
             * kernel doesn't know about sessions any more (they live in
             * root_process_t, roottask side). Servers that genuinely need the
             * caller's session id should ask roottask directly via a
             * proc_get_session(client_pid) message — only tty drivers actually
             * care. See doc/md/rtmux_signal_removal.md for the matching
             * argument re: signal-related fields.
             */
            clp->nd = RTMUX_LOCAL_NODE;
            clp->pid = prp->pid;
            clp->sid = 0;
            clp->flags = 0;
            if (prp->flags & RTMUX_FLG_PROC_STOPPED) {
                clp->flags |= RTMUX_FLG_CI_STOPPED;
            }
            cip = &prp->cred->info;
        }
        ngroups = min_uint((unsigned) kap->ngroups, cip->ngroups);
        memcpy(&clp->cred, cip,
               offsetof(struct _cred_info, grouplist) +
                   ngroups * sizeof(clp->cred.grouplist[0]));
        /* If users passes 0 for number of groups, return the count */
        clp->cred.ngroups = (kap->ngroups) ? ngroups : cip->ngroups;
        if (user_slice_copyout(&ci_slice, clp))
            return EFAULT;
    }

    return EOK;
}


/* Get and optionally modify a connection's flags (connect_flags kcall).
 *
 * Returns the connection's previous RTMUX_FLG_COF_CLOEXEC state and, if
 * kap->mask selects it, updates the CLOEXEC flag on the slot. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (target pid, coid, mask, bits)
 * Return value: ENOERROR on success (old flags in KSTATUS); an errno on
 * failure.
 */
int kdecl ksys_connect_flags(thread_t *act,
                             struct kcall_args_connect_flags *kap)
{
    process_t *prp;
    vector_t *vec;
    void *ptr;
    connect_t *cap_cop;
    connect_t *cop;
    unsigned old;
    unsigned coid;
    uint64_t coid_context;
    int cap_pinned = 0;
    bool prp_rcu;

    if (kap->pid == act->process->pid) {
        prp = act->process;
        prp_rcu = false;
    } else if (kap->pid) {
        prp = lookup_pid_rcu(kap->pid);
        prp_rcu = true;
        if (!prp) {
            rcu_read_unlock(&process_rcu);
            return EPERM;
        }
        if (prp != act->process) {
            rcu_read_unlock(&process_rcu);
            return EPERM;
        }
    } else {
        prp = act->process;
        prp_rcu = false;
    }

    if (prp != act->process)
        return EPERM;

    rcu_read_lock(&connobj_rcu);
    if (endpoint_cap_pin_context(prp, kap->coid, &coid_context, &cap_cop,
                                 &cap_pinned) != EOK) {
        rcu_read_unlock(&connobj_rcu);
        if (prp_rcu)
            rcu_read_unlock(&process_rcu);
        return EBADF;
    }
    /* Lookup connect. */
    vec = (coid_context & RTMUX_SIDE_CHANNEL) ? &prp->chancons : &prp->fdcons;
    coid = (unsigned) coid_context & ~RTMUX_SIDE_CHANNEL;

    /* vec slot read + flag mutation under prp->vec_slock so a concurrent
     * ksys_connect_detach can't vector_rem the slot between our read and the
     * vector_flag write.
     */
    kspinlock_lock(&prp->vec_slock);
    if (coid >= vec->nentries || VECAND(ptr = VEC(vec, coid), 1) ||
        !(cop = VECAND(ptr, ~3UL)) || (cap_cop && cap_cop != cop) ||
        cop->type != TYPE_CONNECTION) {
        kspinlock_unlock(&prp->vec_slock);
        if (cap_pinned)
            (void) rtmux_cap_unpin(&prp->endpoint_caps,
                                   (rtmux_cap_t) kap->coid);
        rcu_read_unlock(&connobj_rcu);
        if (prp_rcu)
            rcu_read_unlock(&process_rcu);
        return EBADF;
    }

    old = VECAND(ptr, 2) ? RTMUX_FLG_COF_CLOEXEC : 0;


    if ((uint32_t) kap->mask & RTMUX_FLG_COF_CLOEXEC) {
        vector_flag(vec, coid,
                    ((uint32_t) kap->bits & RTMUX_FLG_COF_CLOEXEC) ? 1 : 0);
    }
    if ((uint32_t) kap->mask & RTMUX_FLG_COF_REVOKE_SEND) {
        /* One-way clear of the connection's send right (see
         * RTMUX_FLG_COF_REVOKE_SEND). The send hot path (fastmsg.c) and the
         * sigevent delivery path (event.c) read cop->rights locklessly, so
         * clear it with an atomic; cop is held live under prp->vec_slock here.
         */
        (void) __atomic_and_fetch(&cop->rights,
                                  ~(uint32_t) RTMUX_CAP_ENDPOINT_SEND,
                                  __ATOMIC_RELAXED);
    }
    kspinlock_unlock(&prp->vec_slock);
    if (cap_pinned)
        (void) rtmux_cap_unpin(&prp->endpoint_caps, (rtmux_cap_t) kap->coid);
    rcu_read_unlock(&connobj_rcu);
    if (prp_rcu)
        rcu_read_unlock(&process_rcu);

    SETKSTATUS(act, old);
    return ENOERROR;
}
