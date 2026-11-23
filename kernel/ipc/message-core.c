/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <core/globals.h>
#include "reply-client.h"
#include <unistd.h>
#include <user-slice.h>

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
    {
        /* Read once: a channel destroy clears the field at any moment. */
        channel_t *chp = __atomic_load_n(&cop->channel, __ATOMIC_ACQUIRE);

        rep->chid = chp ? chp->chid : -1;
    }
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



/* Steps one viov_to_piov call may take once it has produced a part: one per
 * leaf translation and one per iov entry read. The walk runs with interrupts
 * on, but the scheduler cannot preempt a kernel call, and a timer expiry that
 * readies a higher-priority thread is deferred until the call returns. This
 * caps the non-preemptible span of one msg_readiov call at about 512 KiB of 4
 * KiB pages, whatever the message length or the caller's output capacity.
 */
#define VIOV_STEP_MAX 128

/* Decide whether the walk stops before its next step: once it has a part to
 * return, after VIOV_STEP_MAX steps or on NEED_PREEMPT. The step that makes the
 * first part is free, so every call that gets past the offset makes progress.
 * The entries the offset skips are charged by viov_skip_stop instead.
 */
static inline int viov_stop(thread_t *act, int nparts, unsigned *steps)
{
    return nparts && (++*steps > VIOV_STEP_MAX || NEED_PREEMPT(act));
}

/* Decide whether the skip of one more entry must wait for the next call. Every
 * skipped entry costs a step, so a call spends at most VIOV_STEP_MAX on the
 * entries before the offset, however many there are. NEED_PREEMPT is not
 * consulted: the caller could not use an early stop, since the walk has no part
 * to return and the cursor alone carries the progress.
 */
static inline int viov_skip_stop(unsigned *steps)
{
    return ++*steps > VIOV_STEP_MAX;
}

/* Read and check the caller's cursor. It is user input, so it must describe a
 * position the walk could have stopped at: spare zero, entry within the part
 * count, start not past the offset (the walk only moves forward), and start
 * zero when entry is zero. Anything else is rejected, not clamped. A NULL curp
 * means the start of the iov.
 * Return value: 0, -EFAULT if the cursor could not be read, or -EINVAL.
 */
static int viov_cursor_load(thread_t *act,
                            const struct _iov_cursor *curp,
                            int vparts,
                            size_t voffset,
                            int *entry,
                            size_t *start)
{
    struct _iov_cursor c;

    *entry = 0;
    *start = 0;
    if (!curp)
        return 0;
    if (user_slice_get(act, &c, curp, sizeof(c)))
        return -EFAULT;
    if (c.spare || c.entry > (unsigned) vparts || c.start > voffset ||
        (!c.entry && c.start))
        return -EINVAL;
    *entry = (int) c.entry;
    *start = (size_t) c.start;
    return 0;
}

/* Write the caller's cursor back: entry holds the next unread byte, and start
 * bytes of the iov precede it. A NULL curp writes nothing.
 * Return value: nonzero if the store faulted.
 */
static int viov_cursor_store(thread_t *act,
                             struct _iov_cursor *curp,
                             int entry,
                             size_t start)
{
    struct _iov_cursor c = {.start = start, .entry = (uint32_t) entry};

    return curp && user_slice_put(act, curp, &c, sizeof(c));
}

/* Store a finished physical part as entry i of the validated output slice.
 * Return value: 0, or nonzero if the store faulted.
 */
static inline int piov_store(const struct user_slice *out,
                             int i,
                             const iov_t *part)
{
    return xfer_memcpy((iov_t *) out->base + i, part, sizeof(*part));
}

/* Convert a virtual iov to a physical iov for a process.
 *
 * Walks the virtual iov (after skipping voffset bytes), translating it one leaf
 * mapping at a time and coalescing physically-contiguous runs into as few
 * physical parts as possible. The iov entries and the output array are user
 * memory in act's address space: each range is checked once and then accessed
 * only through fault-recovering copies. Only the buffers the entries describe
 * are translated in prp. The walk can stop early (see viov_stop), so the parts
 * may cover only a prefix of the iov even with output room to spare; the caller
 * continues at voffset plus the bytes the parts cover.
 *
 * Entries before voffset cost one step each (viov_skip_stop). A cursor lets a
 * continuation start at the entry where the previous call stopped, so the skips
 * of a whole message add up to one pass over its entries. Parameters:
 *   act     : the calling thread, whose address space holds viop and piop
 *   prp     : process whose address space the described buffers belong to
 *   piop    : destination physical iov array (user memory, pparts entries)
 *   pparts  : capacity of piop, at least 1
 *   viop    : source virtual iov array (user memory), or, when vparts is
 *             negative, the base of one flat buffer of -vparts bytes
 *   vparts  : number of virtual iov entries, or a negated flat length
 *   voffset : bytes to skip at the start of the virtual iov
 *   curp    : user memory holding a struct _iov_cursor, or NULL. Read before
 *             the walk and updated after it, whatever the result but -EFAULT.
 * Return value: number of physical iov parts produced, 0 only when no bytes
 *               remain past voffset, -EINTR if the skip spent its budget
 *               before a part (cursor advanced; call again), -E2BIG if it did
 *               with no cursor to advance, -EINVAL for a cursor that does not
 *               fit the iov and offset, or -EFAULT if an iov entry could
 *               not be read, names memory outside prp's user range, or a
 *               physical part could not be written.
 */
int viov_to_piov(thread_t *act,
                 process_t *prp,
                 iov_t *piop,
                 int pparts,
                 const iov_t *viop,
                 int vparts,
                 size_t voffset,
                 struct _iov_cursor *curp)
{
    const pid_t pid = prp ? prp->pid : 0;
    const size_t pgsz = vm_ops->pagesize;
    const iov_t *ents = NULL;
    struct user_slice in, out;
    iov_t part, cur;
    int nparts = 0, i, ret;
    size_t estart = 0;
    unsigned steps = 0;

    if (user_slice_init(act, piop, (size_t) pparts * sizeof(*piop),
                        (size_t) pparts * sizeof(*piop), USER_SLICE_WR, &out))
        return -EFAULT;
    if (vparts < 0) {
        /* A flat buffer is its own single entry; nothing to read. */
        SETIOV(&part, viop, PARTS_TO_LEN(vparts));
        vparts = 1;
    } else if (user_slice_init(act, viop, (size_t) vparts * sizeof(*viop),
                               (size_t) vparts * sizeof(*viop), USER_SLICE_RD,
                               &in)) {
        return -EFAULT;
    } else {
        ents = (const iov_t *) in.base;
    }

    if ((ret = viov_cursor_load(act, curp, vparts, voffset, &i, &estart)))
        return ret;
    voffset -= estart;

    for (; i < vparts; i++) {
        uintptr_t vaddr;
        size_t vlen, n;

        if (viov_stop(act, nparts, &steps))
            goto done;
        if (ents && xfer_memcpy(&part, &ents[i], sizeof(part)))
            return -EFAULT;

        /* Skip entries the offset consumes, then start inside the next. Only a
         * walk with no part yet is skipping; after the first part voffset is 0
         * and a zero-length entry is just a step viov_stop charged. A skip that
         * spends the budget leaves entry i for the next call.
         */
        if (voffset >= GETIOVLEN(&part)) {
            if (!nparts && viov_skip_stop(&steps)) {
                ret = curp ? -EINTR : -E2BIG;
                if (viov_cursor_store(act, curp, i, estart))
                    return -EFAULT;
                return ret;
            }
            voffset -= GETIOVLEN(&part);
            estart += GETIOVLEN(&part);
            continue;
        }

        /* The client can rewrite its iov after the send checked it, and a
         * kernel VA would translate to a kernel physical address, so recheck
         * that the entry lies in the client's user range.
         */
        if (prp &&
            user_slice_check((uintptr_t) GETIOVBASE(&part), GETIOVLEN(&part),
                             SIZE_MAX, prp->boundary_addr))
            return -EFAULT;
        vaddr = (uintptr_t) GETIOVBASE(&part) + voffset;
        vlen = GETIOVLEN(&part) - voffset;
        voffset = 0;

        for (; vlen; vlen -= n, vaddr += n) {
            paddr_t paddr;
            size_t leaf;

            if (viov_stop(act, nparts, &steps))
                goto done;

            /* Translate vaddr and bound the step by the bytes left in the leaf
             * that maps it, so a chunk never runs past the end of its frame
             * into whatever frame happens to follow it physically. An unmapped
             * address advances to the next page boundary.
             */
            if (vm_ops->vaddrinfo(pid, prp, vaddr, &paddr, &leaf, VI_NORMAL) ==
                PROT_NONE) {
                paddr = ~(paddr_t) 0;
                leaf = pgsz - (vaddr & (pgsz - 1));
            }
            n = min(vlen, leaf);

            /* An unmapped part (base ~0) never extends: base + len wraps to a
             * small value that a real frame could match.
             */
            if (nparts &&
                (paddr_t) (uintptr_t) GETIOVBASE(&cur) != ~(paddr_t) 0 &&
                paddr ==
                    (paddr_t) (uintptr_t) GETIOVBASE(&cur) + GETIOVLEN(&cur)) {
                SETIOV(&cur, GETIOVBASE(&cur), GETIOVLEN(&cur) + n);
                continue;
            }

            /* Not contiguous with cur: emit cur and start a new part. The last
             * part the caller has room for ends the walk, so the work is
             * bounded by the output capacity and later entries are never read.
             */
            if (nparts && piov_store(&out, nparts - 1, &cur))
                return -EFAULT;
            SETIOV(&cur, (uintptr_t) paddr, n);
            if (++nparts == pparts)
                goto done;
        }
        estart += GETIOVLEN(&part);
    }

done:
    if (nparts && piov_store(&out, nparts - 1, &cur))
        return -EFAULT;
    if (viov_cursor_store(act, curp, i, estart))
        return -EFAULT;
    return nparts;
}


/* Take a vthread off the queue of cop's channel, the send queue or the reply
 * queue by its state, under the channel's lock. The channel is read once: a
 * channel destroy clears cop->channel at any moment, after it has drained both
 * queues, so a vthread found without a channel is on neither.
 */
static void remote_ipc_unqueue(connect_t *cop, thread_t *thp)
{
    channel_t *chp = __atomic_load_n(&cop->channel, __ATOMIC_ACQUIRE);

    if (!chp)
        return;
    kspinlock_lock(&chp->lock);
    switch (thp->state) {
    case STATE_SEND:
    case STATE_NET_SEND:
        pril_rem(&chp->send_queue, thp);
        break;
    default:
        LINKPRIL_REM(thp);
        break;
    }
    kspinlock_unlock(&chp->lock);
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
            remote_ipc_unqueue(cop, thp);
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
        remote_ipc_unqueue(cop, thp);
        thp->state = STATE_STOPPED;

        _TRACE_TH_EMIT_STATE(thp, STOPPED);
    } else {
        /* The remote IPC service reply will decrement it. */
        (void) atomic_add_value(&cop->links, 1);
    }

    /* this is not a client blocked for server, no onbehalf */
    act->args.ms.server = 0;
    act->args.ms.coid = vtid;
    debug_set_blocked_connect(act, cop,
                              __atomic_load_n(&cop->channel, __ATOMIC_ACQUIRE));
    act->state = STATE_REPLY;
    act->restart = 0;

    _TRACE_TH_EMIT_STATE(act, REPLY);
    block();
    act->blocked_on = cop;
    IPC_BLK(act);

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

/* Request identity.
 *
 * An rcvid is the client's thread number and the slot of its connection in the
 * server's table. Both are reused, by the client's next request at once, so a
 * server thread that acts late on an rcvid (a reply for a client that was
 * killed meanwhile, an unblock notification handled after the request was
 * answered) would reach whatever request is blocked under the same number by
 * then. No bit of the rcvid is free to tell them apart, so the identity is kept
 * beside it:
 *
 * - every send takes a new identity (req_gen);
 * - a server thread's receive records the rcvid it returned together with the
 *   identity of the request behind it (rcv_key). An unblock notification
 *   carries the identity of the request it was made for and is recorded the
 *   same way; any other notification leaves the thread holding no rcvid.
 *   MsgCurrent records the request it adopts;
 * - a reply, error, read, write or info call by a thread on the rcvid it
 *   holds, for a request other than the one blocked under it now, is refused
 *   while another thread is serving that request (reply_client_lock).
 *
 * A thread is held to its own last receive and to nothing else; one that was
 * handed an rcvid by another thread is not checked. And the refusal has to
 * leave room for servers that park a request and answer it from whichever
 * thread comes by: the kernel cannot tell a thread acting late on the request
 * it once received from one that was handed the next request under the same
 * rcvid. So a thread whose request was answered by another thread is taken to
 * have handed it on and is released from its record, and a stale holder is
 * refused only while the present request still has a server of its own (the
 * client's server link, which that server's next receive clears). What stays
 * refused is what the bug was: the late use of an rcvid, by the thread that
 * received it or its unblock notification, while another thread is at work on
 * the request that took its place.
 *
 * The record is one word, the rcvid in the low 31 bits and the identity above.
 * That is 33 bits of it: a thread that held a record across 2^33 sends, days of
 * all harts doing nothing else, could meet a request with the same word, and
 * would then not be refused. One word is so that the thread's own receive, a
 * sender on another hart delivering to it while it waits, and another thread's
 * reply releasing it each make one atomic store or exchange. 0 is no rcvid: a
 * connection slot is never 0.
 */
static uint64_t msg_request_seq;

/* One counter for all harts: an identity must differ from every other one a
 * server can still hold, whichever thread object or hart it came from.
 *
 * The unblock state of the last request goes with it. Only a reply or an error
 * removes a queued unblock notification and its mark; a request that ends any
 * other way (a channel destroy drain, a ConnectDetach, a server that is
 * terminating) leaves them set, and a mark left set makes force_ready send no
 * notification for the next request. A force_ready still at work on the
 * last request checks the identity under the channel lock before it marks, so
 * the new identity is published first.
 */
void msg_request_begin(thread_t *sender)
{
    __atomic_store_n(&sender->req_gen,
                     __atomic_add_fetch(&msg_request_seq, 1, __ATOMIC_RELAXED),
                     __ATOMIC_RELEASE);
    ITF_CLR(sender, RTMUX_ITF_UNBLOCK_QUEUED);
    atomic_clr(&sender->flags, RTMUX_FLG_THR_UNBLOCK_REQ);
}

static int msg_request_rcvid(const thread_t *client, const connect_t *cop)
{
    return (int) ((client->tid << 16) | cop->scoid);
}

static uint64_t msg_key(int rcvid, uint64_t gen)
{
    return gen << 31 | (uint32_t) rcvid;
}

static int msg_key_rcvid(uint64_t key)
{
    return (int) (key & 0x7fffffffU);
}

/* server is given client's request, blocked on cop, with its rcvid. */
void msg_request_received(thread_t *server,
                          const thread_t *client,
                          const connect_t *cop)
{
    if (TYPE_MASK(client->type) == TYPE_VTHREAD || (cop->flags & COF_NETCON))
        return; /* numbered otherwise, and not checked */
    __atomic_store_n(&server->rcv_key,
                     msg_key(msg_request_rcvid(client, cop), client->req_gen),
                     __ATOMIC_RELEASE);
}

/* server is given a notification. An unblock notification names rcvid and was
 * made for the request gen; any other one names no request.
 */
void msg_notification_received(thread_t *server,
                               int code,
                               int rcvid,
                               uint64_t gen)
{
    __atomic_store_n(
        &server->rcv_key,
        code == _NOTIFICATION_CODE_UNBLOCK && gen ? msg_key(rcvid, gen) : 0,
        __ATOMIC_RELEASE);
}

/* replier has answered client's request (a reply or an error), with client
 * claimed. What replier held of it is spent. If another thread is down as its
 * server, that one handed it on: release it too, if this request is still what
 * it holds.
 */
void msg_request_answered(thread_t *replier,
                          const thread_t *client,
                          const connect_t *cop)
{
    uint64_t key, expect;
    thread_t *server;

    if (TYPE_MASK(client->type) == TYPE_VTHREAD || (cop->flags & COF_NETCON))
        return;
    key = msg_key(msg_request_rcvid(client, cop), client->req_gen);
    expect = key;
    (void) __atomic_compare_exchange_n(&replier->rcv_key, &expect, 0, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);

    /* The link is cleared by the server's teardown; the guard keeps a thread
     * read from it allocated, as in thread_disown_client().
     */
    rcu_read_lock(&tthread_rcu);
    server = __atomic_load_n(&client->args.ms.server, __ATOMIC_ACQUIRE);
    if (server && server != replier) {
        expect = key;
        (void) __atomic_compare_exchange_n(&server->rcv_key, &expect, 0, false,
                                           __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
    }
    rcu_read_unlock(&tthread_rcu);
}

/* Whether the active thread may act on client's request under its rcvid: not
 * when it holds that rcvid for another request and somebody else is serving
 * this one.
 */
static bool msg_request_current(const thread_t *client, const connect_t *bound)
{
    const thread_t *act = get_active_thread(get_cpunum());
    const thread_t *server;
    uint64_t key;
    int rcvid;

    if (TYPE_MASK(client->type) == TYPE_VTHREAD || (bound->flags & COF_NETCON))
        return true;
    rcvid = msg_request_rcvid(client, bound);
    key = __atomic_load_n(&act->rcv_key, __ATOMIC_ACQUIRE);
    if (msg_key_rcvid(key) != rcvid || key == msg_key(rcvid, client->req_gen))
        return true;
    server = __atomic_load_n(&client->args.ms.server, __ATOMIC_ACQUIRE);
    return !server || server == act;
}

/* Lock the channel that serves the reply-blocked client an rcvid named.
 *
 * Returns the connection the client is blocked on, with *chpp locked, or NULL
 * with nothing locked when the rcvid no longer names such a client (ESRCH).
 *
 * The rcvid lookup keeps received and client allocated, and its RCU-sched
 * section (open until the kcall leaves the kernel) keeps a channel that was
 * read from received->channel allocated: ksys_channel_destroy clears that field
 * before its rcu_sched_synchronize() and object_free(). It does not keep the
 * connection attached: a destroy on another hart drains the reply queue,
 * unpublishes the connection and clears the field at any moment, so the field
 * is read once and a NULL fails the call. A client found here in STATE_REPLY
 * and blocked on the connection is on chp's reply queue, since the sender links
 * itself under chp->lock and the destroy drain unlinks it under the same lock
 * before it detaches the connection. That holds for a client blocked on a
 * virtual connection of received as well: it is attached to the channel of its
 * master (ksys_connect_attach) and detached with it.
 *
 * A sender stores STATE_REPLY, its queue link and blocked_on in separate steps
 * bracketed by WAIT_PENDING, and its receiver can run and use the rcvid on
 * another hart before the last one, so wait the bracket out first.
 */
connect_t *reply_client_lock(connect_t *received,
                             thread_t *client,
                             channel_t **chpp)
{
    channel_t *chp;
    connect_t *bound;

    while (client->internal_flags & RTMUX_ITF_WAIT_PENDING)
        __asm__ volatile("" ::: "memory");
    cpu_smp_mb();
    inject_reply_claim_stall(received);
    chp = __atomic_load_n(&received->channel, __ATOMIC_ACQUIRE);
    if (!chp)
        return NULL;
    kspinlock_lock(&chp->lock);
    if ((client->state == STATE_REPLY || client->state == STATE_NET_REPLY) &&
        !(client->internal_flags & RTMUX_ITF_SPECRET_PENDING) &&
        (bound = reply_client_connection(client, received)) != NULL &&
        msg_request_current(client, bound)) {
        *chpp = chp;
        return bound;
    }
    kspinlock_unlock(&chp->lock);
    return NULL;
}

/* Take the MSG_DELIVERY claim on a client found by reply_client_lock(), with
 * the channel lock still held. A client some other path has claimed is not
 * taken: the bit has one owner, and setting it twice lets the first release
 * expose the second owner's copy. The bit is set in every configuration, so
 * that it excludes the claimers from each other even where force_ready does not
 * read it (no CONFIG_SMP_MSGOPT).
 */
bool reply_client_claim_locked(thread_t *client)
{
    if (client->internal_flags & RTMUX_ITF_MSG_DELIVERY)
        return false;
    ITF_SET(client, RTMUX_ITF_MSG_DELIVERY);
    return true;
}

/* Claim the reply-blocked client an rcvid named, for a reply, an error or a
 * follow-up copy: reply_client_lock(), the claim, and the unlock. While the
 * claim is held force_ready defers to the claimer and the channel cannot finish
 * its destroy drain, so *chpp and *copp stay usable until msg_claim_release()
 * or the caller's own unlink.
 *
 * Returns EOK with *copp the connection the client is blocked on; ESRCH when
 * the rcvid no longer names a reply-blocked client; or EAGAIN when the client
 * is there but another thread's reply, error or follow-up copy holds it. That
 * claim lasts no longer than one kernel entry of its holder, and failing the
 * call would lose a reply that is still deliverable, so act's call has been
 * restarted (KCALL_RESTART) to find the client free, or gone, on a later entry:
 * the caller only returns.
 */
int reply_client_claim(thread_t *act,
                       connect_t **copp,
                       thread_t *client,
                       channel_t **chpp)
{
    connect_t *bound = reply_client_lock(*copp, client, chpp);
    bool taken;

    if (!bound)
        return ESRCH;
    taken = reply_client_claim_locked(client);
    kspinlock_unlock(&(*chpp)->lock);
    if (!taken) {
        KCALL_RESTART(act);
        return EAGAIN;
    }
    *copp = bound;
    return EOK;
}

/* Give up a MSG_DELIVERY claim on a peer without completing the exchange, and
 * deliver a wake that force_ready deferred to the claimer meanwhile.
 * force_ready takes the scheduler lock, which ranks below the channel lock, so
 * it runs after the unlock.
 */
void msg_claim_release(channel_t *chp, thread_t *peer)
{
    unsigned status = 0;
    bool wake = false;

    kspinlock_lock(&chp->lock);
    ITF_CLR(peer, RTMUX_ITF_MSG_DELIVERY);
    if (peer->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
        ITF_CLR(peer, RTMUX_ITF_MSG_FORCE_RDY);
        status = (unsigned) KSTATUS(peer);
        wake = true;
    }
    kspinlock_unlock(&chp->lock);
    if (wake)
        force_ready(peer, status);
}

/* A receiving server can disown its previous client as that client times out or
 * is taken over by another server. Read the link inside the lifetime guard and
 * clear only a reverse link that still names this server.
 */
bool rdecl thread_disown_client(thread_t *srv)
{
    bool ended = false;
    if (!__atomic_load_n(&srv->client, __ATOMIC_ACQUIRE))
        return false;
    rcu_read_lock(&tthread_rcu);
    thread_t *client = __atomic_load_n(&srv->client, __ATOMIC_ACQUIRE);
    if (client) {
        thread_t *expect = srv;
        (void) __atomic_compare_exchange_n(&client->args.ms.server, &expect,
                                           NULL, false, __ATOMIC_ACQ_REL,
                                           __ATOMIC_RELAXED);
        ended = thread_client_departed(srv, client);
    }
    rcu_read_unlock(&tthread_rcu);
    return ended;
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
        thp->short_msg_source = act;
        atomic_or(&thp->flags, RTMUX_FLG_THR_SHORT_MSG);

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
        RECEIVER_TAKEN(thp);
        kspinlock_unlock(&chp->lock);

        /* By default the receiver runs with message driven priority: the remote
         * sender lends prio (a NET_REPLY client, so not a request loan that a
         * reply ends; the next receive replaces it). thp is on no queue until
         * block_and_ready() below.
         */
        thread_lend_msg(
            thp, (chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0 ? prio : 0,
            false);
        thread_prio_settle(thp);

        /* this is not a client blocked for server, no onbehalf */
        act->args.ms.server = 0;

        /* Block the active thread and ready the receiver thread */
        act->state =
            STATE_NET_REPLY; /* Must be set before calling block_and_ready() */

        _TRACE_TH_EMIT_STATE(act, NET_REPLY);
        block_and_ready(thp);
        act->blocked_on = cop;
        IPC_BLK(act);

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
    IPC_BLK(act);

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
        IPC_BLK(act);
        SETKSTATUS(act, -ESRCH);
        ready(act);
        return ESRCH;
    }
    pril_add(&chp->send_queue, act);
    kspinlock_unlock(&chp->lock);
    (void) atomic_add_value(&cop->links, 1);

    /* To prevent priority inversion, lend the pending send to the threads
     * serving the channel until they next receive or reply.
     */
    if ((chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0)
        channel_lend_pending(chp, prio, true);

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
