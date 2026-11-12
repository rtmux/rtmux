/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include <unistd.h>

#define SIP_ROTL(x, b) (((x) << (b)) | ((x) >> (64 - (b))))
#define SIP_ROUND(v0, v1, v2, v3) \
    do {                          \
        v0 += v1;                 \
        v1 = SIP_ROTL(v1, 13);    \
        v1 ^= v0;                 \
        v0 = SIP_ROTL(v0, 32);    \
        v2 += v3;                 \
        v3 = SIP_ROTL(v3, 16);    \
        v3 ^= v2;                 \
        v0 += v3;                 \
        v3 = SIP_ROTL(v3, 21);    \
        v3 ^= v0;                 \
        v2 += v1;                 \
        v1 = SIP_ROTL(v1, 17);    \
        v1 ^= v2;                 \
        v2 = SIP_ROTL(v2, 32);    \
    } while (0)

/* SipHash-2-4 over a byte buffer under the 128-bit key (k0, k1). Parameters:
 *   p, n   : the bytes
 *   k0, k1 : key halves
 * Return value: the 64-bit tag.
 */
static uint64_t siphash24(const unsigned char *p,
                          size_t n,
                          uint64_t k0,
                          uint64_t k1)
{
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0;
    uint64_t v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0;
    uint64_t v3 = 0x7465646279746573ULL ^ k1;
    uint64_t b = (uint64_t) n << 56;
    size_t i;

    for (; n >= 8; n -= 8, p += 8) {
        uint64_t m = 0;

        for (i = 0; i < 8; i++)
            m |= (uint64_t) p[i] << (8 * i);
        v3 ^= m;
        SIP_ROUND(v0, v1, v2, v3);
        SIP_ROUND(v0, v1, v2, v3);
        v0 ^= m;
    }
    for (i = 0; i < n; i++)
        b |= (uint64_t) p[i] << (8 * i);
    v3 ^= b;
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    v0 ^= b;
    v2 ^= 0xff;
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    SIP_ROUND(v0, v1, v2, v3);
    return v0 ^ v1 ^ v2 ^ v3;
}

/* Boot-wide key halves for keydata tags and the entropy pool the per-thread
 * secrets are drawn from. The kernel has no hardware RNG, so the pool is fed
 * from timing and address jitter on every draw and ratcheted through SipHash;
 * see docs/capability-abi.md for what that does and does not give. Updates are
 * lock-free: a lost race only costs a little pool state, and the per-call
 * sample (cycle counter, time base, cpu, caller) keeps concurrent draws apart.
 */
static uint64_t keydata_pool[2] = {0x9e3779b97f4a7c15ULL,
                                   0xbf58476d1ce4e5b9ULL};
static uint64_t keydata_ctr;

/* Draw a fresh nonzero 64-bit per-thread secret. Parameters:
 *   salt : caller-specific bits mixed into the sample
 * Return value: the secret.
 */
uint64_t keydata_draw(uintptr_t salt)
{
    unsigned char smp[40];
    uint64_t s0 = __atomic_load_n(&keydata_pool[0], __ATOMIC_RELAXED);
    uint64_t s1 = __atomic_load_n(&keydata_pool[1], __ATOMIC_RELAXED);
    uint64_t v[5], out;
    unsigned i, j;

    v[0] = clock_cycles();
    v[1] = cpu_clock_ticks();
    v[2] = (uint64_t) qtimeptr->nsec;
    v[3] = (uint64_t) salt ^ ((uint64_t) get_cpunum() << 56);
    v[4] = __atomic_add_fetch(&keydata_ctr, 1, __ATOMIC_RELAXED);
    for (i = 0; i < 5; i++)
        for (j = 0; j < 8; j++)
            smp[i * 8 + j] = (unsigned char) (v[i] >> (8 * j));
    out = siphash24(smp, sizeof smp, s0, s1);
    smp[0] ^= 0x5a;
    __atomic_store_n(&keydata_pool[0], siphash24(smp, sizeof smp, s1, s0),
                     __ATOMIC_RELAXED);
    __atomic_store_n(&keydata_pool[1], out ^ s0, __ATOMIC_RELAXED);
    return out ? out : 1;
}

/* Mix the current cycle count into the pool; called on every keydata kcall so
 * unprivileged callers' timing also stirs it.
 */
void keydata_stir(void)
{
    __atomic_fetch_xor(&keydata_pool[0], clock_cycles() * 0x9e3779b97f4a7c15ULL,
                       __ATOMIC_RELAXED);
}

/* Compute the 32-bit wire key for a message under a thread's 64-bit secret. The
 * tag is SipHash-2-4 keyed by (secret, boot-wide pool half), truncated to 32
 * bits and never 0. Parameters:
 *   buf, n : the message bytes (a kernel copy)
 *   secret : the thread's secret
 * Return value: the key.
 */
uint32_t rdecl keygen(const unsigned char *buf, size_t n, uint64_t secret)
{
    uint32_t k = (uint32_t) siphash24(buf, n, secret, 0x726d75782d6b6579ULL);

    return k ? k : 1;
}


/* Fill a _msg_info structure describing a received message.
 *
 * Reports source/destination node, pid, channel/connection ids, message lengths
 * and priority, distinguishing local from remote IPC (vthread) connections, and
 * flags pending unblock or remote credential changes. Parameters:
 *   thp : the sending thread (or vthread)
 *   tid : caller-supplied tid, or -1 to use thp->tid
 *   cop : the connection the message came in on
 *   rep : destination _msg_info structure
 */
void rdecl get_rcvinfo(thread_t *thp,
                       int tid,
                       connect_t *cop,
                       struct _msg_info *rep)
{
    process_t *prp;

#ifndef NDEBUG
    if (!thp)
        crash();
#endif

    /* thp->process == cop->process if we used remote_ipc_send() */
    if ((cop->flags & COF_NETCON) && TYPE_MASK(thp->type) == TYPE_VTHREAD) {
        rep->srcnd = thp->un.remote.srcnd;
        rep->nd = cop->un.remote.nd;
        rep->pid = cop->un.remote.pid;
        if ((rep->flags = (cop->un.remote.flags & RTMUX_MI_ENDIAN_BIG))) {
            /* Remote is big-endian, we are little-endian */
            rep->flags |= RTMUX_MI_ENDIAN_DIFF;
        }
    } else {
        rep->srcnd = cop->un.lcl.nd;
        rep->nd = RTMUX_LOCAL_NODE;
        rep->pid = cop->process->pid;
        rep->flags = 0;
    }
    rep->chid = cop->channel ? cop->channel->chid : -1;
    rep->scoid = cop->scoid | RTMUX_SIDE_CHANNEL;
    rep->msglen = thp->args.ms.msglen;
    rep->srcmsglen = (thp->flags & RTMUX_FLG_THR_BUFF_MSG)
                         ? thp->args.ms.msglen
                         : thp->args.ms.srcmsglen;
    rep->dstmsglen = thp->args.ms.dstmsglen;
    rep->coid = (int32_t) thp->args.ms.coid;
    if (rtmux_cap_get_or_alloc_context(&thp->process->endpoint_caps, cop,
                                       (uint64_t) thp->args.ms.coid,
                                       cop->rights, &rep->endpoint_cap) != EOK)
        rep->endpoint_cap = 0;
    rep->endpoint_rights = cop->rights;
    rep->priority = thp->priority;
    if (tid == -1) {
        tid = (int) thp->tid;
    }
    rep->tid = tid + 1;

    /* Is there an unblock pending on the thread? */
    if (thp->flags & RTMUX_FLG_THR_UNBLOCK_REQ) {
        rep->flags |= RTMUX_MI_UNBLOCK_REQ;
    }

    /* In the case of Net doing an info request we let him know if the process
     * credentials have changed.
     */
    if (TYPE_MASK(thp->type) == TYPE_THREAD) {
        prp = thp->process;

        if (get_active_thread(get_cpunum())->process == remote_ipc.prp &&
            rep->chid == remote_ipc.chp->chid &&
            (cop->flags & COF_NETCON) == 0) {
            if (cop->flags & COF_VCONNECT) {
                cop = cop->un.lcl.cop;
            }
            if (prp->seq != cop->un.lcl.seq) {
                cop->un.lcl.seq = prp->seq;
                rep->flags |= RTMUX_MI_NET_CRED_DIRTY;
            }

            /* The thread's msg_key_data secret must not reach even the net
             * manager; the remote key travels via vtp->keydata instead.
             */
            rep->msglen = 0;
        }
    }
}



/* Convert a virtual iov to a physical iov for a process.
 *
 * Walks the virtual iov (after skipping voffset bytes), translating each page
 * to its physical address and coalescing physically-contiguous runs into as few
 * physical parts as possible. Parameters:
 *   prp     : process whose address space the virtual iov belongs to
 *   piop    : destination physical iov array
 *   pparts  : capacity of piop
 *   viop    : source virtual iov array
 *   vparts  : number of virtual iov entries
 *   voffset : bytes to skip at the start of the virtual iov
 * Return value: number of physical iov parts produced.
 */
int viov_to_piov(process_t *prp,
                 iov_t *piop,
                 int pparts,
                 iov_t *viop,
                 int vparts,
                 int voffset)
{
    char *vaddr;
    paddr_t paddr;
    size_t vlen, n;
    int nparts;

    /* Skip over the offset specified in the virtual iov. */
    while ((voffset -= (int) GETIOVLEN(viop)) >= 0) {
        ++viop;
        if (--vparts == 0) {
            return 0;
        }
    }

    /* Set vaddr and vlen to point within the current viop entry. voffset is
     * negative here: negate to get unsigned byte count
     */
    vlen = (size_t) (-voffset);
    vaddr = (GETIOVLEN(viop) - vlen) + (char *) GETIOVBASE(viop);

    /* Step through each virtual part producing one or more physical parts. */
    for (nparts = 0; pparts;) {
        for (; pparts && vlen; vlen -= n, vaddr += n) {
            /* Limit steps to 1 page. */
            n = min(vlen, vm_ops->pagesize);

            /* Convert virtual addr to physical addr. */
            if (vm_ops->vaddrinfo(prp ? prp->pid : 0, prp, (uintptr_t) vaddr,
                                  &paddr, NULL, VI_NORMAL) == PROT_NONE) {
                paddr = ~(paddr_t) 0;
            }
            /* Check if address is contiguous. */
            if (nparts && paddr == (paddr_t) (uintptr_t) GETIOVBASE(piop - 1) +
                                       GETIOVLEN(piop - 1)) {
                SETIOV(piop - 1, GETIOVBASE(piop - 1), GETIOVLEN(piop - 1) + n);
                continue;
            }
            /* Page not contiguous in physical memory so create new part. */
            SETIOV(piop, (intptr_t) paddr, n);
            ++piop;
            --pparts;
            ++nparts;
        }

        /* Finished current virtual part. Any more parts? */
        if (--vparts == 0) {
            break;
        }
        /* Move to next virtual part. */
        ++viop;
        vaddr = GETIOVBASE(viop);
        vlen = GETIOVLEN(viop);
    }

    return nparts;
}


/* Forward a kernel message operation to the remote IPC service.
 *
 * Packages the active thread's msg_read/Write/Reply/Error/DeliverEvent request
 * for the remote side and reply-blocks the active thread on the remote IPC
 * service channel, which performs the actual data transfer. Repairs vthread
 * queue state when that channel is gone (channel destroy).
 *
 * The server ends up reply-blocked on the remote IPC thread, which will
 * msgread/msgwrite the data in/out. Parameters:
 *   kap  : kernel-call arguments for the originating syscall
 *   vtid : virtual thread id on the remote node
 *   cop  : the remote IPC connection
 *   thp  : the target thread (server/vthread)
 * Return value: cop for __KSYS_MSG_CURRENT, otherwise NULL (with an errno
 *               set on the active thread on failure).
 */
connect_t *rdecl remote_ipc_send(kcall_args_t *kap,
                                 unsigned vtid,
                                 connect_t *cop,
                                 thread_t *thp)
{
    thread_t *act = get_active_thread(get_cpunum());
    int type = (int) KTYPE(act);
    int status;
    unsigned prio;

    if (!remote_ipc.chp) {
        /* The remote IPC service is either dead or never started. For the dead
         * case, repair vthreads.
         */
        if (type == __KSYS_CHANNEL_DESTROY) {
            if (thp->args.ms.server != 0) {
                thp->args.ms.server->client = 0;
                thp->args.ms.server = 0;
            }
            /* Queue mutation under the per-channel lock */
            kspinlock_lock(&cop->channel->lock);
            switch (thp->state) {
            case STATE_SEND:
            case STATE_NET_SEND:
                /* Remove vthread from send queue */
                pril_rem(&cop->channel->send_queue, thp);
                break;
            default:
                /* Remove vthread from reply queue */
                LINKPRIL_REM(thp);
                break;
            }
            kspinlock_unlock(&cop->channel->lock);
            thp->state = STATE_STOPPED;
            snap_time(&thp->timestamp_last_block, 0);

            _TRACE_TH_EMIT_STATE(thp, STOPPED);
        }
        kcall_err(act, EREMOTE);
        return NULL;
    }

    switch (type) {
    case __KSYS_MSG_VERIFY_EVENT:
        /* @@@ Not supported yet */
        kcall_err(act, EREMOTE);
        return NULL;

    case __KSYS_MSG_DELIVER_EVENT:
    case __KSYS_CHANNEL_DESTROY:
        /* These are the only two that don't need a reply blocked connection */
        break;

    default:
        if (thp->state != STATE_REPLY || thp->blocked_on != cop) {
            kcall_err(act, ESRCH);
            return NULL;
        }
    }

    act->args.ms.rparts = 0;

    switch (type) {
    case __KSYS_MSG_READV:
        act->args.ms.srcmsglen =
            (uint32_t) xferlen(act, kap->msg_readv.rmsg, kap->msg_readv.rparts);
        act->args.ms.rmsg = kap->msg_readv.rmsg;
        act->args.ms.rparts = kap->msg_readv.rparts;
        act->args.ms.sparts = 0;
        act->args.ms.msglen = kap->msg_readv.offset;
        break;

    case __KSYS_MSG_READWRITEV:
        kcall_err(act, EREMOTE);
        return NULL;

    case __KSYS_MSG_WRITEV:
        act->args.ms.srcmsglen = (uint32_t) xferlen(act, kap->msg_writev.smsg,
                                                    kap->msg_writev.sparts);
        act->args.ms.smsg = kap->msg_writev.smsg;
        act->args.ms.sparts = kap->msg_writev.sparts;
        act->args.ms.msglen = kap->msg_writev.offset;
        break;

    case __KSYS_MSG_REPLYV:
        act->args.ms.srcmsglen = (uint32_t) xferlen(act, kap->msg_replyv.smsg,
                                                    kap->msg_replyv.sparts);
        act->args.ms.smsg = kap->msg_replyv.smsg;
        act->args.ms.sparts = kap->msg_replyv.sparts;
        act->args.ms.msglen = (uint32_t) kap->msg_replyv.status;
        break;

    case __KSYS_MSG_DELIVER_EVENT:
        act->args.msbuff.msglen = sizeof *kap->msg_deliver_event.event;
        memcpy(act->args.msbuff.buff, kap->msg_deliver_event.event,
               sizeof *kap->msg_deliver_event.event);
        atomic_or((volatile unsigned *) &act->flags, RTMUX_FLG_THR_BUFF_MSG);
        break;

    case __KSYS_CHANNEL_DESTROY:
        act->args.msbuff.msglen = sizeof(int);
        *(int *) act->args.msbuff.buff = ESRCH;
        atomic_or((volatile unsigned *) &act->flags, RTMUX_FLG_THR_BUFF_MSG);
        type = __KSYS_MSG_ERROR;
        break;

    case __KSYS_MSG_ERROR:
        act->args.msbuff.msglen = sizeof kap->msg_error.err;
        memcpy(act->args.msbuff.buff, &kap->msg_error.err,
               sizeof kap->msg_error.err);
        atomic_or((volatile unsigned *) &act->flags, RTMUX_FLG_THR_BUFF_MSG);
        break;

    case __KSYS_MSG_CURRENT:
        return cop;

    default:
        crash();
    }

    if (type == __KSYS_MSG_REPLYV || type == __KSYS_MSG_ERROR) {
        if (thp->args.ms.server != 0) {
            thp->args.ms.server->client = 0;
            thp->args.ms.server = 0;
        }
        /* Queue mutation under the per-channel lock */
        kspinlock_lock(&cop->channel->lock);
        switch (thp->state) {
        case STATE_SEND:
        case STATE_NET_SEND:
            /* Remove vthread from send queue */
            pril_rem(&cop->channel->send_queue, thp);
            break;
        default:
            /* Remove vthread from reply queue */
            LINKPRIL_REM(thp);
            break;
        }
        kspinlock_unlock(&cop->channel->lock);
        thp->state = STATE_STOPPED;

        _TRACE_TH_EMIT_STATE(thp, STOPPED);
    } else {
        /* The remote IPC service reply will decrement it. */
        (void) atomic_add_value(&cop->links, 1);
    }

    /* this is not a client blocked for server, no onbehalf */
    act->args.ms.server = 0;
    act->args.ms.coid = vtid;
    act->state = STATE_REPLY;
    act->restart = 0;

    _TRACE_TH_EMIT_STATE(act, REPLY);
    block();
    act->blocked_on = cop;

    /* LINKPRIL_BEG on remote_ipc.chp->reply_queue under remote_ipc.chp->lock.
     * Released before notification_deliver / force_ready (both take SCHED).
     */
    kspinlock_lock(&remote_ipc.chp->lock);
    LINKPRIL_BEG(remote_ipc.chp->reply_queue, act, thread_t);
    kspinlock_unlock(&remote_ipc.chp->lock);

    prio = act->priority;
    if (prio > NUM_PRI - 1)
        prio = NUM_PRI - 1;
    if ((status = notification_deliver(
             remote_ipc.chp, prio | _NOTIFICATION_PRIO_VTID, type,
             (act->tid << 16) | (cop->un.remote.coid & 0xffff), (intptr_t) act,
             0)) != EOK) {
        force_ready(act, (unsigned) status | _FORCE_SET_ERROR);
    }

    return NULL;
}


/* Prime a virtual thread with remote IPC send parameters.
 *
 * Looks up the vthread by id and stamps the connection id, priority, key data
 * and source node from the incoming _vtid_info. Parameters:
 *   vtid : virtual thread id
 *   vtp  : remote-supplied send parameters
 * Return value: the matching vthread_t.
 */
vthread_t *rdecl remote_ipc_vthread(unsigned vtid, struct _vtid_info *vtp)
{
    vthread_t *thp;
    unsigned prio;

    thp = vector_lookup(&vthread_vector, vtid);

    /* Fail closed on an out-of-range/empty vtid instead of dereferencing NULL:
     * the guard must stay live in release builds too (the caller checks NULL).
     */
    if (!thp || thp->un.remote.vtid != vtid)
        return NULL;

    thp->args.ms.coid = vtp->coid;
    prio = vtp->priority;
    if (prio > NUM_PRI - 1)
        prio = NUM_PRI - 1;
    thp->priority = (uint8_t) prio;
    thp->key = (uint32_t) vtp->keydata; /* opaque key from remote */
    thp->un.remote.srcnd = vtp->srcnd;

    return thp;
}


/* Remove a queued unblock notification for a thread from a channel.
 *
 * Walks the connection's channel send_queue under the per-channel lock and
 * frees any _NOTIFICATION_CODE_UNBLOCK notification matching rcvid, then clears
 * the thread's UNBLOCK_QUEUED flag. No SCHED primitive runs inside the loop
 * (object_free on notification_souls only), so no drop-and-reacquire is needed.
 * Lock-rank: SCHED < CHANNEL.
 *
 * Callers MUST NOT hold channel_slock on entry. Parameters:
 *   thp   : thread whose UNBLOCK_QUEUED flag is cleared
 *   cop   : connection whose channel queue is scanned
 *   rcvid : rcvid identifying the unblock notification to remove
 */
void rdecl remove_unblock(thread_t *thp, connect_t *cop, int rcvid)
{
    notification_t *pup, *pup2;
    channel_t *chp = cop->channel;

    if (!chp) {
        return;
    }

    kspinlock_lock(&chp->lock);
    for (pup = pril_first(&chp->send_queue); pup; pup = pup2) {
        pup2 = pril_next(pup);
        if ((TYPE_MASK(pup->type) == TYPE_NOTIFICATION) &&
            (pup->code == _NOTIFICATION_CODE_UNBLOCK) &&
            (pup->value == rcvid)) {
            /* channel_slock above serializes the queue walk + pril_rem. */
            pril_rem(&chp->send_queue, pup);
            object_free(chp->process, &notification_souls, pup);
        }
    }
    kspinlock_unlock(&chp->lock);

    atomic_clr((volatile unsigned *) &thp->internal_flags,
               RTMUX_ITF_UNBLOCK_QUEUED);
}

/* Send a buffered remote IPC message from a vthread to a local channel.
 *
 * If a receiver is waiting, hands it the buffered message and reply- blocks the
 * active remote IPC thread; otherwise queues the send on the channel and boosts
 * the server's threads to avoid priority inversion. Parameters:
 *   act  : the active remote IPC thread carrying the buffered message
 *   cop  : the connection to the local channel
 *   prio : message-driven priority
 * Return value: ENOERROR on success, or an errno (EBADF/ENXIO).
 */
int rdecl remote_ipc_msg(thread_t *act, connect_t *cop, unsigned prio)
{
    channel_t *chp;
    thread_t *thp;

    /* Get dst channel. */
    if (!(chp = cop->channel)) {
        return EBADF;
    }

    act->args.msbuff.rmsg = (void *) -1;
    act->args.msbuff.rparts = 0;

    atomic_or((volatile unsigned *) &act->flags, RTMUX_FLG_THR_BUFF_MSG);

    /* chp->receive_queue scan + LINKPRIL_REM under channel_slock. Released
     * before block_and_ready (SCHED < CHANNEL rank order) and reacquired for
     * LINKPRIL_BEG on chp->reply_queue.
     */
    kspinlock_lock(&chp->lock);
    if (chp->destroying || cop->channel != chp) {
        kspinlock_unlock(&chp->lock);
        atomic_clr((volatile unsigned *) &act->flags, RTMUX_FLG_THR_BUFF_MSG);
        return ESRCH;
    }
    thp = chp->receive_queue;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    while ((thp) && (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY)) {
        thp = thp->next.thread;
    }
#endif
    if ((thp) && !(thp->internal_flags & RTMUX_ITF_RCVNOTIFICATION)) {
        atomic_or(&thp->flags, RTMUX_FLG_THR_SHORT_MSG);
        thp->blocked_on = act;

        /* Indicate that there is a receiver depending on data in our thread_t
         * object.
         */
        ITF_SET(act, RTMUX_ITF_SPECRET_PENDING);

        SETKSTATUS(thp, -((act->tid << 16) | cop->scoid));

        thp->restart = NULL;
        if (thp->args.ri.info) {
            thp->args.ri.cop = cop;
            thp->args.ri.thp = act;
            atomic_or(&thp->flags, RTMUX_FLG_THR_RCVINFO);
        }
        /* Unlink receive thread from the receive queue. */
        LINKPRIL_REM(thp);
        kspinlock_unlock(&chp->lock);

        /* By default the receiver runs with message driven priority. */
        if ((chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
            thp->real_priority = thp->priority = (uint8_t) prio;
        }

        /* this is not a client blocked for server, no onbehalf */
        act->args.ms.server = 0;

        /* Block the active thread and ready the receiver thread */
        act->state =
            STATE_NET_REPLY; /* Must be set before calling block_and_ready() */

        _TRACE_TH_EMIT_STATE(act, NET_REPLY);
        block_and_ready(thp);
        act->blocked_on = cop;

        /* Link the now reply blocked sending thread in the reply queue.
         *
         * No ->destroying recheck here (unlike the send-block path below): the
         * common wake for a reply-blocked sender is the server's
         * ksys_msg_replyv via rcvid, independent of this channel. chp is valid
         * across this link (RCU-sched), but that section ends once the sender
         * context-switches away, so in the narrow window where a
         * channel_destroy links this sender after its reply drain (channel.c)
         * has passed, chp can be freed with the sender still linked. That stale
         * reply_queue linkage is made memory-safe by force_ready's
         * STATE_REPLY->RECEIVE unlink, which skips LINKPRIL_REM when the owning
         * channel is gone (dispatch.c). The residual is liveness only: such a
         * sender waits until its own teardown wakes it.
         */
        kspinlock_lock(&chp->lock);
        LINKPRIL_BEG(chp->reply_queue, act, thread_t);
        kspinlock_unlock(&chp->lock);
        (void) atomic_add_value(&cop->links, 1);

        return ENOERROR;
    }
    kspinlock_unlock(&chp->lock);

    if (chp->process->flags & (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_ZOMBIE |
                               RTMUX_FLG_PROC_COREDUMP)) {
        return ENXIO;
    }
    act->state = STATE_NET_SEND; /* Must be set before calling block() */
    _TRACE_TH_EMIT_STATE(act, NET_SEND);
    block();
    act->blocked_on = cop;

    /* pril_add on chp->send_queue under chp->lock. Recheck ->destroying in the
     * same window, mirroring the send-block guard in ksys_msg_sendv: a
     * concurrent channel_destroy sets ->destroying under chp->lock before it
     * drains send_queue, so if we see it clear our entry is guaranteed visible
     * to that drain; if we see it set, the drain may already have passed and
     * queueing now would strand this sender forever (no receiver ever arrives
     * on a dead channel — unlike a reply-blocked sender, nothing wakes it via
     * rcvid). block() already took act off the run queue but it is not yet on
     * send_queue, so simply re-ready it.
     */
    kspinlock_lock(&chp->lock);
    if (chp->destroying || cop->channel != chp) {
        kspinlock_unlock(&chp->lock);

        /* Abandon the buffered send. Clear RTMUX_FLG_THR_BUFF_MSG (set
         * unconditionally above) so the re-readied thread is not mistaken for
         * one still carrying send data.
         *
         * block() already reselected the active thread away from act, so on the
         * trap-return path act->state != STATE_RUNNING and the handler skips
         * writing our ESRCH into act's status register. Deliver it explicitly,
         * exactly as force_ready does for a drained sender: SETKSTATUS(thp,
         * -err). Without this, act would resume with the entry-zeroed status
         * and read a spurious success from a send to a destroyed channel.
         */
        atomic_clr((volatile unsigned *) &act->flags, RTMUX_FLG_THR_BUFF_MSG);
        act->blocked_on = NULL;
        SETKSTATUS(act, -ESRCH);
        ready(act);
        return ESRCH;
    }
    pril_add(&chp->send_queue, act);
    kspinlock_unlock(&chp->lock);
    (void) atomic_add_value(&cop->links, 1);

    /* To prevent priority inversion, boost all threads in the server */
    if ((chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
        unsigned i;

        for (i = 0; i < chp->process->threads.nentries; ++i) {
            if (VECP(thp, &chp->process->threads, i) && thp->priority < prio &&
                thp->last_chid == chp->chid) {
                adjust_priority(thp, prio, thp->dpp, 1);
                thp->real_priority = thp->priority;
            }
        }
    }

    return ENOERROR;
}

/* Build a notification in the active thread's message buffer and send it over a
 * remote IPC connection. Parameters:
 *   act   : the active remote IPC thread
 *   cop   : the remote IPC connection
 *   coid  : connection id to record in the buffered message
 *   prio  : notification priority (must be 1..NUM_PRI-1)
 *   code  : notification code
 *   value : notification value
 * Return value: result of remote_ipc_msg, or EINVAL on bad priority.
 */
int rdecl remote_ipc_notify(thread_t *act,
                            connect_t *cop,
                            unsigned coid,
                            unsigned prio,
                            int code,
                            int value)
{
    struct _notification *p;

    prio = 0xff & prio;
    if (prio == 0 || prio > NUM_PRI - 1) {
        return EINVAL;
    }
    act->args.msbuff.coid = coid;
    act->args.msbuff.msglen = sizeof(int) + sizeof(struct _notification);
    act->args.msbuff.dstmsglen = __KSYS_MSG_SEND_NOTIFICATION;
    *(unsigned *) (&act->args.msbuff.buff[0]) = prio;
    p = (struct _notification *) &act->args.msbuff.buff[sizeof(int)];
    p->type = _NOTIFICATION_TYPE;
    p->subtype = _NOTIFICATION_SUBTYPE;
    p->code = (int8_t) code;
    p->value.sival_int = value;
    p->scoid = cop->scoid;

    return remote_ipc_msg(act, cop, prio);
}
