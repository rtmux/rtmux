/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <sys/iomsg.h>
#include <core/globals.h>

/* New message passing code. Faster version than original -- optimizes for the
 * common I/O service cases:
 * - process to process msgpass
 * - small messages (less than 128-256 bytes) or...
 * - larger messages with multi-iov_t
 * - normally, offsets into copy routine are zero
 * - msginfo is requested
 * - we rarely get send-blocked
 * - send and reply length are requested
 *
 * It improves performance by doing the following For short messaged:
 * - dramatically increases the range of messages handled through the
 *   short message path (from 32 up to about 256 bytes)
 * - allows the handling of multi-iov_t short messages
 * - the 2 changes above typically allow handling of 95%+ of send and
 *   80%+ of replies in the fast path (it was less than 50% with old code)
 * Large messages are improved as follows:
 * - make a copy of the incoming IOVs before blocking (up to a max)
 * - the fast copy routine now integrates the as map and copy in one
 * - we branch off early after checking that we meet conditions above
 *
 * We also remove the specret processing if possible. This leads to some
 * simplifications, and also solves some of the issues with the non-atomicity of
 * the message xfer code in the kernel (a reply to a thread sneaking in between
 * the receive and specret path, for example)
 */

/* memcpy routines inlined — arch-specific override via runtime.h */

#ifndef __inline_xfer_memcpy
#define __inline_xfer_memcpy(a, b, c) memcpy(a, b, c)
#endif

#ifndef NO_INLINE_BLOCKANDREADY
#if CONFIG_PROCESSORS_MAX <= 1
#define INLINE_BLOCKANDREADY
#endif
#endif

/* Fast path rcvinfo stuffing for the local case with no pending unblock */

#define RCVINFO_ENDIAN_FLAG (0)

static void fastmsg_fill_rcvinfo(thread_t *thp,
                                 connect_t *cop,
                                 struct _msg_info *rep)
{
    if ((cop->flags & COF_NETCON && TYPE_MASK(thp->type) == TYPE_VTHREAD) ||
        (thp->process == remote_ipc.prp &&
         cop->channel->chid == remote_ipc.chp->chid &&
         (cop->flags & COF_NETCON) == 0)) {
        get_rcvinfo(thp, -1, cop, rep);
    } else {
        rep->nd = RTMUX_LOCAL_NODE;
        rep->srcnd = cop->un.lcl.nd;
        rep->pid = cop->process->pid;
        rep->tid = (int32_t) (thp->tid + 1);
        rep->chid = cop->channel->chid;
        rep->scoid = cop->scoid | RTMUX_SIDE_CHANNEL;
        rep->coid = (int32_t) thp->args.ms.coid;
        rep->msglen = thp->args.ms.msglen;
        rep->srcmsglen = (thp->flags & RTMUX_FLG_THR_BUFF_MSG)
                             ? thp->args.ms.msglen
                             : thp->args.ms.srcmsglen;
        rep->dstmsglen = thp->args.ms.dstmsglen;
        if (rtmux_cap_get_or_alloc_context(
                &thp->process->endpoint_caps, cop, (uint64_t) thp->args.ms.coid,
                cop->rights, &rep->endpoint_cap) != EOK)
            rep->endpoint_cap = 0;
        rep->endpoint_rights = cop->rights;
        rep->priority = thp->priority;
        rep->flags = RCVINFO_ENDIAN_FLAG;
    }
}

static int fastmsg_copy_rcvinfo(thread_t *dst_thp,
                                struct _msg_info *dst,
                                const struct _msg_info *src)
{
    return xfer_cpy_diov(dst_thp, (iov_t *) (void *) dst,
                         (uint8_t *) (void *) src, -(int) sizeof(*src),
                         sizeof(*src));
}

typedef struct fastmsg_native_cap_tx {
    struct _io_native_hdr *hdr;
    rtmux_cap_t original[RTMUX_CAP_TRANSFER_MAX];
    rtmux_cap_t installed[RTMUX_CAP_TRANSFER_MAX];
    uint16_t nxfers;
} fastmsg_native_cap_tx_t;

/* A reply is raw payload unless the client opted in (RTMUX_ITF_NATIVE_REPLY,
 * checked by ksys_msg_replyv) and its server built a native header: a read
 * reply is file data, and any 8 bytes of data can spell type == _IO_MSG and
 * protocol == _IOMSG_RTMUX_NATIVE (two RISC-V instruction words do). A request
 * always opens with an I/O header, so a native request with a wrong version is
 * a protocol error (EINVAL); the same bytes at the head of an opted-in reply
 * are data and pass through unparsed.
 */
static bool fastmsg_native_hdr_is_data(const struct _io_native_hdr *hdr,
                                       bool reply)
{
    return reply && hdr->version != _IO_NATIVE_VERSION;
}

static int fastmsg_native_cap_install_buffer(thread_t *sender,
                                             thread_t *receiver,
                                             uint8_t *msg,
                                             size_t msglen,
                                             bool reply,
                                             fastmsg_native_cap_tx_t *tx)
{
    struct _io_native_hdr *hdr;
    rtmux_cap_transfer_t xfers[RTMUX_CAP_TRANSFER_MAX];
    rtmux_cap_t out[RTMUX_CAP_TRANSFER_MAX];
    bool all_optional = true;
    uint16_t i;
    int ret;

    tx->hdr = NULL;
    tx->nxfers = 0;
    if (msglen < sizeof(*hdr))
        return EOK;

    hdr = (struct _io_native_hdr *) (void *) msg;
    if (hdr->msg.type != _IO_MSG || hdr->msg.protocol != _IOMSG_RTMUX_NATIVE)
        return EOK;
    if (fastmsg_native_hdr_is_data(hdr, reply))
        return EOK;
    if (hdr->version != _IO_NATIVE_VERSION)
        return EINVAL;
    if (hdr->nxfers == 0)
        return EOK;
    if (hdr->nxfers > _IO_NATIVE_CAP_TRANSFER_MAX ||
        hdr->nxfers > RTMUX_CAP_TRANSFER_MAX)
        return E2BIG;

    for (i = 0; i < hdr->nxfers; i++) {
        tx->original[i] = hdr->xfers[i].id;
        xfers[i].cap = tx->original[i];
        xfers[i].rights = hdr->xfers[i].rights;

        /* The OPTIONAL bit lives in the native-transfer flags but must not
         * reach rtmux_cap_transfer_to, which requires a zero flags field.
         */
        xfers[i].flags =
            hdr->xfers[i].flags & ~(uint32_t) _IO_NATIVE_CAP_XFER_OPTIONAL;
        if ((hdr->xfers[i].flags & _IO_NATIVE_CAP_XFER_OPTIONAL) == 0)
            all_optional = false;
    }
    ret = rtmux_cap_transfer_to(&sender->process->endpoint_caps,
                                &receiver->process->endpoint_caps, xfers, out,
                                hdr->nxfers);
    if (ret != EOK) {
        /* An all-optional transfer that will not fit in the receiver's table is
         * dropped, not fatal: the reply is delivered with no caps installed
         * (nxfers cleared) rather than failing. Any required transfer still
         * fails the reply.
         */
        if (ret == ENOSPC && all_optional) {
            hdr->nxfers = 0;
            return EOK;
        }
        return ret;
    }

    for (i = 0; i < hdr->nxfers; i++)
        hdr->xfers[i].id = out[i];
    tx->hdr = hdr;
    tx->nxfers = hdr->nxfers;
    for (i = 0; i < tx->nxfers; i++)
        tx->installed[i] = out[i];
    return EOK;
}

static void fastmsg_native_cap_rollback(thread_t *receiver,
                                        fastmsg_native_cap_tx_t *tx)
{
    uint16_t i;

    if (tx->nxfers == 0)
        return;

    for (i = 0; i < tx->nxfers; i++) {
        if (tx->hdr)
            tx->hdr->xfers[i].id = tx->original[i];
        (void) rtmux_cap_abort(&receiver->process->endpoint_caps,
                               tx->installed[i]);
    }
    tx->hdr = NULL;
    tx->nxfers = 0;
}

static int fastmsg_copy_source_prefix(thread_t *sender,
                                      void *dst,
                                      size_t want,
                                      uint32_t *copied)
{
    iov_t *iov;
    char *out = dst;
    size_t n;
    int parts;
    int ret;

    *copied = 0;
    parts = sender->args.ms.sparts;
    if (parts == 0 || want == 0)
        return EOK;

    if (parts < 0) {
        n = min_size(want, PARTS_TO_LEN(parts));
        ret = xfer_memcpy(out, sender->args.ms.smsg, n);
        if (ret != 0)
            return ret;
        *copied = (uint32_t) n;
        return EOK;
    }

    iov = sender->args.ms.smsg;
    while (parts-- > 0 && *copied < want) {
        n = min_size(want - *copied, GETIOVLEN(iov));
        ret = xfer_memcpy(out + *copied, GETIOVBASE(iov), n);
        if (ret != 0)
            return ret;
        *copied += (uint32_t) n;
        iov++;
    }
    return EOK;
}

static int fastmsg_native_cap_install_long(thread_t *sender,
                                           thread_t *receiver,
                                           bool reply,
                                           fastmsg_native_cap_tx_t *tx,
                                           int *patched)
{
    struct _io_msg msg;
    struct _io_native_hdr hdr;
    iov_t *rmsg;
    int32_t rparts;
    uint32_t copied;
    int ret;
    unsigned cpu;
    thread_t *active;
    process_t *active_as;

    *patched = 0;
    tx->hdr = NULL;
    tx->nxfers = 0;

    cpu = get_cpunum();
    active = get_active_thread(cpu);
    active_as = active->as_prp ? active->as_prp : active->process;
    rmsg = receiver->args.ms.rmsg;
    rparts = receiver->args.ms.rparts;

    if (active == sender) {
        if (pcpu_array[cpu].pc_as_prp != active_as)
            vm_ops->as(active_as, &pcpu_array[cpu].pc_as_prp);
        ret = fastmsg_copy_source_prefix(sender, &msg, sizeof(msg), &copied);
    } else {
        receiver->args.ms.rmsg = (iov_t *) (void *) &msg;
        receiver->args.ms.rparts = -(int32_t) sizeof(msg);
        ret = xfermsg(receiver, sender, 0, 0);
        copied = sender->args.ms.msglen;
        receiver->args.ms.rmsg = rmsg;
        receiver->args.ms.rparts = rparts;
        receiver->restart = NULL;
        sender->restart = NULL;
        sender->args.ms.msglen = 0;
        sender->args.ms.dthp = NULL;
    }
    if (pcpu_array[cpu].pc_as_prp != active_as)
        vm_ops->as(active_as, &pcpu_array[cpu].pc_as_prp);
    if (ret != 0)
        return ret;
    if (copied < sizeof(msg) || msg.type != _IO_MSG ||
        msg.protocol != _IOMSG_RTMUX_NATIVE)
        return EOK;

    if (active == sender) {
        ret = fastmsg_copy_source_prefix(sender, &hdr, sizeof(hdr), &copied);
    } else {
        receiver->args.ms.rmsg = (iov_t *) (void *) &hdr;
        receiver->args.ms.rparts = -(int32_t) sizeof(hdr);
        ret = xfermsg(receiver, sender, 0, 0);
        copied = sender->args.ms.msglen;
        receiver->args.ms.rmsg = rmsg;
        receiver->args.ms.rparts = rparts;
        receiver->restart = NULL;
        sender->restart = NULL;
        sender->args.ms.msglen = 0;
        sender->args.ms.dthp = NULL;
    }
    if (pcpu_array[cpu].pc_as_prp != active_as)
        vm_ops->as(active_as, &pcpu_array[cpu].pc_as_prp);
    if (ret != 0)
        return ret;
    if (copied < sizeof(hdr))
        return reply
                   ? EOK
                   : EINVAL; /* a short reply is data, as in the buffer path */
    if (fastmsg_native_hdr_is_data(&hdr, reply))
        return EOK;
    if (hdr.nxfers == 0)
        return EOK;

    ret = fastmsg_native_cap_install_buffer(sender, receiver, (uint8_t *) &hdr,
                                            sizeof(hdr), reply, tx);
    if (ret != EOK)
        return ret;
    tx->hdr = NULL;
    ret = xfer_cpy_diov(receiver, rmsg, (uint8_t *) &hdr, rparts, sizeof(hdr));
    if (ret != 0) {
        fastmsg_native_cap_rollback(receiver, tx);
        return ret;
    }
    *patched = 1;
    return EOK;
}

#define STUFF_RCVINFO(thp, cop, info) fastmsg_fill_rcvinfo(thp, cop, info)

/* send_queue mixes thread senders with queued notifications, and only a thread
 * has internal_flags. A notification overlays that field with its value/id, so
 * testing RTMUX_ITF_MSG_DELIVERY on one reads payload bits: a disconnect notice
 * whose scoid was odd looked "claimed" and was skipped by every receive, then
 * sat in the queue holding its soul forever. Only thread entries can be
 * claimed.
 */
static inline int send_entry_claimed(const thread_t *thp)
{
    unsigned t = TYPE_MASK(thp->type);

    return t != TYPE_NOTIFICATION && t != TYPE_VNOTIFICATION &&
           (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY);
}

/* Called with chp->lock held. Notifications share the queue with senders, but a
 * notification-only receiver must not retry for an ordinary message.
 */
static thread_t *receive_candidate(channel_t *chp, int notification_only)
{
    thread_t *thp = pril_first(&chp->send_queue);

    while (thp) {
        unsigned type = TYPE_MASK(thp->type);
        if (!send_entry_claimed(thp) &&
            (!notification_only || type == TYPE_NOTIFICATION ||
             type == TYPE_VNOTIFICATION))
            return thp;
        thp = thp->next.thread;
    }
    return NULL;
}


#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
/* internal_flags is written from several harts (see ITF_SET in macros.h): the
 * peer's word may take a concurrent WAIT_PENDING / MSG_FORCE_RDY update from
 * another hart while a transfer runs, so every update here is an atomic RMW. A
 * plain |= or &= ~ reads the word, then writes it back, discarding that update
 * (a lost MSG_FORCE_RDY leaves the aborted thread waiting forever).
 */
#define START_SMP_XFER(thp1, thp2)                          \
    {                                                       \
        *(volatile void **) &((thp1)->blocked_on) = (thp2); \
        ITF_SET((thp1), RTMUX_ITF_MSG_DELIVERY);            \
        ITF_SET((thp2), RTMUX_ITF_MSG_DELIVERY);            \
    }

#define END_SMP_XFER(thp1, thp2)                         \
    {                                                    \
        ITF_CLR((thp2), RTMUX_ITF_MSG_DELIVERY);         \
        ITF_CLR((thp1), RTMUX_ITF_MSG_DELIVERY);         \
        *(volatile void **) &((thp1)->blocked_on) = (0); \
    }

/* Drop only the active thread's side of the transfer. The peer keeps its
 * MSG_DELIVERY claim until the caller's final unlink, so a concurrent channel
 * destroy cannot unlink and ready the peer while the caller's tail still runs.
 */
#define END_SMP_XFER_SELF(thp1)                          \
    {                                                    \
        ITF_CLR((thp1), RTMUX_ITF_MSG_DELIVERY);         \
        *(volatile void **) &((thp1)->blocked_on) = (0); \
    }

#else
#define START_SMP_XFER(act, thp)
#define END_SMP_XFER(act, thp)
#define END_SMP_XFER_SELF(act)
#endif

/* Handle CPU vm_ops->as implementations that need the kernel locked */
#define SWITCH_AS(as_prp, asp, act) vm_ops->as(as_prp, asp)

static
#if defined(__GNUC__) || defined(__INTEL_COMPILER)
    __inline__
#endif

    /* Look up a connection by coid in a process's fd or side-channel vector.
     *
     * Parameters:
     *   prp  : process whose connection vectors to search
     *   coid : connection id (RTMUX_SIDE_CHANNEL bit selects chancons)
     * Return value: the connect_t, or NULL if the slot is empty/invalid.
     */
    connect_t *rdecl
    inline_lookup_connect(process_t *prp, uint64_t coid, uint64_t *context)
{
    vector_t *vec;
    connect_t *cop = NULL;
    void *ptr;

    if (context)
        *context = coid;
    if (rtmux_cap_generation((rtmux_cap_t) coid) != 0) {
        void *object = NULL;
        uint64_t cap_context = coid;

        if (rtmux_cap_lookup_pin_context(
                &prp->endpoint_caps, (rtmux_cap_t) coid,
                RTMUX_CAP_ENDPOINT_SEND, &object, &cap_context) != EOK)
            return NULL;
        vec =
            (cap_context & RTMUX_SIDE_CHANNEL) ? &prp->chancons : &prp->fdcons;
        kspinlock_lock(&prp->vec_slock);
        cop = vector_lookup2(vec, (unsigned) cap_context & ~RTMUX_SIDE_CHANNEL);
        if (cop != object)
            cop = NULL;
        kspinlock_unlock(&prp->vec_slock);
        (void) rtmux_cap_unpin(&prp->endpoint_caps, (rtmux_cap_t) coid);
        if (context)
            *context = cap_context;
        return cop;
    }

    if (coid & RTMUX_SIDE_CHANNEL) {
        coid &= ~RTMUX_SIDE_CHANNEL;
        vec = &prp->chancons;
    } else {
        vec = &prp->fdcons;
    }
    if (!vec)
        return NULL;

    /* Without vec_slock, a concurrent vector_add growth path can swap
     * vec->vector to a freshly _scalloc'd buffer and _sfree the old one while
     * we're mid-read. The result is observing the old buffer's contents after
     * they've been returned to the heap — chp pointers derived from those reads
     * look stale and ksys_msg_sendv falls into the chp->flags ENOSYS branch.
     *
     * Take vec_slock for the brief read. Same lock guards the mutator in
     * lifecycle.c / channel.c / connect.c.
     */
    kspinlock_lock(&prp->vec_slock);
    /* Do unsigned compare with 'coid' to catch negative values as well */
    if (coid < vec->nentries &&
        VECAND(ptr = VEC(vec, (unsigned) coid), 1) == 0) {
        cop = VECAND(ptr, ~3);
    }
    kspinlock_unlock(&prp->vec_slock);

    return cop;
}

/* Send a message on a connection (msg_sendv kcall).
 *
 * Validates the send/reply iovs, then either hands the message directly to a
 * waiting receiver (transferring data and reply-blocking the sender) or, with
 * no receiver waiting, send-blocks the sender on the channel's send queue and
 * boosts server-thread priorities to avoid inversion. Parameters:
 *   act : the calling (active) thread (the sender)
 *   kap : kcall args (coid, send iov smsg/sparts, reply iov rmsg/rparts)
 * Return value: ENOERROR (reply status reported via KSTATUS); an errno on
 * failure.
 */
static int sendv_body(thread_t *act,
                      struct kcall_args_msg_sendv *kap,
                      connect_t **heldp)
{
    connect_t *cop;
    channel_t *chp;
    int type = (int) KTYPE(act);
    thread_t *thp;
    thread_t *sender;
    process_t *actprp = act->process;
    unsigned th_flags = 0;
    uint32_t remote_srcmsglen = -1U;

    /* These are the usual incoming checks
     * - validate connection
     * - get channel pointer
     * - check for cancellation
     */

    /* Open a connobj_rcu read section for the cop/chp deref window. vec_slock
     * around the vector_lookup (in inline_lookup_connect) protects the
     * vector_add/grow path, but once we drop vec_slock a concurrent
     * ksys_channel_destroy can vector_rem + rcu_sched_synchronize
     * + object_free the chp out from under us, leaving cop->channel pointing at
     * recycled heap memory whose first 16 bits are no longer the channel's
     * flags.
     *
     * lookup_rcvid_open() uses the same per-CPU nesting guard as lookup_rcvid;
     * lookup_rcvid_release at trap exit closes it.
     */
    lookup_rcvid_open();

    /* Lookup src connect. */
    {
        uint64_t cap_context = kap->coid;

        if (!(cop = inline_lookup_connect(actprp, kap->coid, &cap_context)) ||
            cop->type != TYPE_CONNECTION) {
            return EBADF;
        }
        kap->coid = cap_context;
    }
    if ((__atomic_load_n(&cop->rights, __ATOMIC_RELAXED) &
         RTMUX_CAP_ENDPOINT_SEND) == 0)
        return EBADF;
    /* Get dst channel. */
    if (!(chp = cop->channel)) {
        return EBADF;
    }

    _TRACE_COMM_EMIT_SMSG(act, cop, (act->tid << 16) | cop->scoid);

    if (PENDCAN(TLS_FLAGS_SAFE(act)) && (type != __KSYS_MSG_SENDVNC)) {
        SETKIP_FUNC(act, act->process->canstub);
        return ENOERROR;
    }

    /* The base conditions are now met. If this is a remote IPC connection or a
     * global channel, handle it separately.
     */
    if (chp->flags & RTMUX_FLG_CHF_GLOBAL) {
        /* Global channels not supported in RTMux */
        return ENOSYS;
    }

    /* Take the sender's reference before the sender is published on any queue
     * (the publishing sites below hand it to the queue by clearing *heldp). The
     * wrapper drops it again on every path that does not publish. A sibling's
     * ConnectDetach of the last fd then either sees our reference (its drop is
     * not the last) or ran first, and links == 0 fails the send.
     */
    if (!connect_link_get(cop))
        return EBADF;
    *heldp = cop;

    sender = act;

    /* Store incoming args */
    if (cop->flags & COF_NETCON) {
        RD_PROBE_INT(act, kap->rmsg, sizeof(struct _vtid_info) / sizeof(int));
        sender = (thread_t *) (void *) remote_ipc_vthread(
            (unsigned) kap->rparts, (struct _vtid_info *) (void *) kap->rmsg);
        if (!sender) {
            return EINVAL;
        }
        if (sender->state != STATE_STOPPED)
            crash();
        sender->args.ms.rmsg = kap->rmsg;
        sender->args.ms.rparts = kap->rparts;
        act->args.ms.smsg = kap->smsg;
        act->args.ms.sparts = kap->sparts;
        /* Do this up-front while we have addressabilty */
        remote_srcmsglen =
            (uint32_t) ((struct _vtid_info *) (void *) kap->rmsg)->srcmsglen;
    } else {
        sender->args.ms.coid = (uint32_t) kap->coid;
        sender->args.ms.rmsg = kap->rmsg;
        sender->args.ms.rparts = kap->rparts;
    }

    atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
    /* Make sure the SPECRET_PENDING bit isn't set when we don't need it. */
    ITF_CLR(sender, RTMUX_ITF_SPECRET_PENDING);

    /* Validate incoming IOVs - override for remote IPC - rparts/rmsg have
     * special meaning
     */
    if (cop->flags & COF_NETCON) {
        sender->args.ms.dstmsglen =
            (uint32_t) ((struct _vtid_info *) (void *) kap->rmsg)->dstmsglen;
    } else if (kap->rparts >= 0) {
        size_t len = 0;
        size_t len_last = 0;
        iov_t *iov = kap->rmsg;
        int rparts = kap->rparts;
        int niov = 0;

        /* Incoming reply iov_t -- make copy of reply IOVs Calculate reply
         * length -- even if not requested, it is almost free Also do boundary
         * check
         */
        while (rparts) {
            uintptr_t base, last;

            len += GETIOVLEN(iov);
            if (len < len_last) {
                /* overflow. excessively long user iov_t, possibly overlayed.
                 * pr62575
                 */
                return EOVERFLOW;
            }
            len_last = len;
            base = (uintptr_t) GETIOVBASE(iov);
            last = base + GETIOVLEN(iov) - 1;
            if (((base > last) ||
                 !WITHIN_BOUNDARY(base, last,
                                  sender->process->boundary_addr)) &&
                (GETIOVLEN(iov) != 0)) {
                return EFAULT;
            }
            /* Keep copy of iov_t */
            if (niov < _NUM_CACHED_REPLY_IOV) {
                /*  sender->args.ms.riov[niov] = *iov; */
            }
            ++iov;
            ++niov;
            --rparts;
        }
        sender->args.ms.dstmsglen = (uint32_t) len;
    } else {
        /* Single part -- validate and store reply address */
        uintptr_t base, last;
        base = (uintptr_t) kap->rmsg;
        last = base + (unsigned) PARTS_TO_LEN(kap->rparts) - 1;
        if ((base > last) ||
            !WITHIN_BOUNDARY(base, last, sender->process->boundary_addr)) {
            /* We know length is non-zero from test above */
            return EFAULT;
        }
        sender->args.ms.dstmsglen = (uint32_t) PARTS_TO_LEN(kap->rparts);
    }


    /* Send IOVs */
    if (kap->sparts < 0) {
        /* Single part -- do the boundary check and copy if short message */
        uintptr_t base, last;
        int len;

        base = (uintptr_t) kap->smsg;
        len = (int) PARTS_TO_LEN(kap->sparts);
        last = base + (unsigned) len - 1;
        if ((base > last) ||
            !WITHIN_BOUNDARY(base, last, sender->process->boundary_addr)) {
            /* We know length is non-zero from test above */
            return EFAULT;
        }
        sender->args.ms.srcmsglen = (uint32_t) len;

        if (len <= sizeof(sender->args.msbuff.buff)) {
            (void) __inline_xfer_memcpy(
                sender->args.msbuff.buff, (char *) base,
                sender->args.msbuff.msglen = (uint32_t) len);
            th_flags = RTMUX_FLG_THR_BUFF_MSG;
        }
    } else if (kap->sparts == 1) {
        /* Single iov_t -- do the boundary check and copy if short message */
        uintptr_t base, last, len;

        base = (uintptr_t) GETIOVBASE(kap->smsg);
        len = GETIOVLEN(kap->smsg);
        last = base + len - 1;
        if (((base > last) ||
             !WITHIN_BOUNDARY(base, last, sender->process->boundary_addr)) &&
            (len != 0)) {
            return EFAULT;
        }
        sender->args.ms.srcmsglen = (uint32_t) len;
        if (len <= sizeof(sender->args.msbuff.buff)) {
            (void) __inline_xfer_memcpy(
                sender->args.msbuff.buff, (char *) base,
                sender->args.ms.msglen = (uint32_t) len);
            th_flags = RTMUX_FLG_THR_BUFF_MSG;
        }
    } else {
        /* Multi iov_t case */
        size_t len = 0;
        size_t len_last = 0;
        iov_t *iov = kap->smsg;
        int sparts = kap->sparts;

        /* Calculate send length -- even if not requested, it is almost free
         * Also do boundary check
         */
        while (sparts) {
            uintptr_t base, last;

            len += GETIOVLEN(iov);
            if (len < len_last) {
                /* overflow. excessively long user iov_t, possibly overlayed.
                 * pr62575
                 */
                return EOVERFLOW;
            }
            len_last = len;
            base = (uintptr_t) GETIOVBASE(iov);
            last = base + GETIOVLEN(iov) - 1;
            if (((base > last) ||
                 !WITHIN_BOUNDARY(base, last,
                                  sender->process->boundary_addr)) &&
                (GETIOVLEN(iov) != 0)) {
                return EFAULT;
            }
            ++iov;
            --sparts;

            /* TODO: cache iov_t copy for deferred send (only needed if no
             * receiver) if(niov < _NUM_CACHED_SEND_IOV) {
             * sender->args.ms.siov[niov] = *iov; }
             */
        }
        sender->args.ms.srcmsglen = (uint32_t) len;
        if (len <= sizeof(sender->args.msbuff.buff)) {
            size_t pos = 0;
            iov = kap->smsg;
            sparts = kap->sparts;

            /* Multi-iov_t incoming message that is short FIXME -- need
             * memcpy_siov for efficiency
             */
            while (sparts) {
                size_t ilen = GETIOVLEN(iov);
                __inline_xfer_memcpy(&sender->args.msbuff.buff[pos],
                                     GETIOVBASE(iov), ilen);

                pos += ilen;
                iov++;
                sparts--;
            }
            sender->args.ms.msglen = (uint32_t) len;
            th_flags = RTMUX_FLG_THR_BUFF_MSG;
        }
    }

    /* Now that the up-front business is done, we do the actual copy. If this
     * was identified as a short message, we have copied the message into the
     * msgbuff area.
     */

    /* Was there was a waiting thread on the channel? */

    /* Probe chp->receive_queue under chp->lock and atomically *claim* the
     * chosen thp by setting RTMUX_ITF_MSG_DELIVERY before releasing the lock.
     * The claim has two effects:
     *   - other concurrent senders skip thp (the loop below already
     *     skips MSG_DELIVERY entries),
     *   - force_ready on thp defers via force_ready_msgxfer instead
     *     of LINKPRIL_REM'ing it under us (see dispatch.c
     *     STATE_RECEIVE force_ready),
     * so the LINKPRIL_REM at the end of the receiver-found path can no longer
     * find thp's prev.pril cleared under it. The claim is held until the
     * post-xfer LINKPRIL_REM completes, then cleared explicitly AFTER
     * LINKPRIL_REM. The long-message path ends its own side with
     * END_SMP_XFER_SELF, which leaves thp's claim in place.
     */
    kspinlock_lock(&chp->lock);
    if (chp->destroying || cop->channel != chp) {
        kspinlock_unlock(&chp->lock);
        return ESRCH;
    }
    thp = chp->receive_queue;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    while ((thp) && (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY)) {
        thp = thp->next.thread;
    }
    if (thp && !(thp->internal_flags & RTMUX_ITF_RCVNOTIFICATION)) {
        ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    }
#endif
    kspinlock_unlock(&chp->lock);

    if (thp) {
        unsigned __wp_spins = 0;
        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING) {
            /* Tight spin: a normal in-flight wait-state transition clears
             * within a handful of iterations. If it never clears (the hang
             * caught at this line under gdb), flood the red in-kernel marker so
             * the wedge is visible on the console. Gated so the normal
             * transient case stays silent.
             */
            if ((++__wp_spins & 0xFFFFFU) == 0)
                MARK_POTN_INF_LOOP();
            __asm__ volatile("" ::: "memory");
        }
        cpu_smp_mb();
    }
    if ((thp) && !(thp->internal_flags & RTMUX_ITF_RCVNOTIFICATION)) {
        fastmsg_native_cap_tx_t captx;
        int xferstat;
        /* If an immediate timeout was specified we return immediately. */
        if (IMTO(act, STATE_REPLY)) {
            atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)

            /* Release the MSG_DELIVERY claim acquired at the peek before
             * returning — thp stays on the queue, free for the next sender to
             * pick up.
             */
            ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
            return ETIMEDOUT;
        }
        /* Is this a long message? */
        if (th_flags == 0) {
            int cap_patched = 0;

            sender->args.ms.smsg = kap->smsg;
            sender->args.ms.sparts = kap->sparts;
            thp->args.ms.rmsg = thp->args.ri.rmsg;
            thp->args.ms.rparts = thp->args.ri.rparts;
            xferstat = fastmsg_native_cap_install_long(sender, thp, false,
                                                       &captx, &cap_patched);
            if (xferstat) {
                if (xferstat & XFER_SRC_FAULT)
                    goto send_fault;
                if (xferstat & XFER_DST_FAULT)
                    goto rcv_fault;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
                ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
                return xferstat;
            }
            START_SMP_XFER(act, thp);
            /* Yes. Transfer the data. */
            xferstat = xfermsg(thp, act,
                               cap_patched ? sizeof(struct _io_native_hdr) : 0,
                               cap_patched ? sizeof(struct _io_native_hdr) : 0);
            sender->args.ms.msglen = act->args.ms.msglen;
            if (cap_patched)
                sender->args.ms.msglen +=
                    (uint32_t) sizeof(struct _io_native_hdr);

            /* thp's MSG_DELIVERY claim must stay set until the LINKPRIL_REM
             * further below; END_SMP_XFER_SELF drops only act's side (the full
             * END_SMP_XFER would clear thp's too and re-open the race a
             * concurrent force_ready could win).
             */
            END_SMP_XFER_SELF(act);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)

            if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
                /* MSG_FORCE_RDY means a deferred wakeup is queued; proceed with
                 * force_ready and KCALL_RESTART. Drop our delivery claim and
                 * the flag together under chp->lock first (as replyv does), so
                 * a later claimer's FORCE_RDY is not cleared after the fact;
                 * force_ready will LINKPRIL_REM thp under chp->lock itself.
                 */
                kspinlock_lock(&chp->lock);
                ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY | RTMUX_ITF_MSG_FORCE_RDY);
                kspinlock_unlock(&chp->lock);
                force_ready(thp, MSG_FORCE_ERROR(thp));
                KCALL_RESTART(act);
                act->restart = 0;
                return ENOERROR;
            }
            if (act->flags &
                (RTMUX_FLG_THR_SIG_ACTIVE | RTMUX_FLG_THR_CANCELSELF)) {
                /* send is a cancelation point Release MSG_DELIVERY claim —
                 * restart will re-peek and re-claim.
                 */
                ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
                KCALL_RESTART(act);
                act->restart = 0;
                return ENOERROR;
            }
#endif

            if (xferstat) {
                if (cap_patched)
                    fastmsg_native_cap_rollback(thp, &captx);

                /* If sender faulted let him know and abort the operation
                 * without waking up the receiver.
                 */
                if (xferstat & XFER_SRC_FAULT) {
                    goto send_fault;
                }

                /* If receiver faulted, wake him up with an error and fail the
                 * send.
                 */
                goto rcv_fault;
            }
        } else {
            /* Short message. We do the following:
             * - switch as to receiver
             */
            xferstat = fastmsg_native_cap_install_buffer(
                sender, thp, sender->args.msbuff.buff,
                sender->args.msbuff.msglen, false, &captx);
            if (xferstat) {
                atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
                ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
                return xferstat;
            }
            if (thp->as_prp &&
                thp->as_prp != pcpu_array[get_cpunum()].pc_as_prp) {
                /* Switch to the receiver's address space for the short-message
                 * copy below. The in-kernel-call preemption fence covers the
                 * path.
                 */
                SWITCH_AS(thp->as_prp, &pcpu_array[get_cpunum()].pc_as_prp,
                          act);
            }
            /* - copy message and handle errors */
            if ((xferstat = xfer_cpy_diov(
                     thp, thp->args.ri.rmsg, sender->args.msbuff.buff,
                     thp->args.ri.rparts, sender->args.msbuff.msglen))) {
                /* Has to be a receiver fault; */
                fastmsg_native_cap_rollback(thp, &captx);
                goto rcv_fault;
            }
            atomic_or(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);

            /* Note: below this point, we should NOT reference kap anywhere as
             * kap points to the original as
             */
        }


        /* If the receive specified an info buffer stuff it as well. However, we
         * are not in the address space of the destination thread, we switch now
         */
        thp->restart = NULL;

        if (thp->args.ri.info) {
            struct _msg_info rep;

            /* Fill in rcvinfo Switch to as of receiver. It's already adjusted
             * if short msg.
             */
            if (th_flags == 0) {
                if (thp->as_prp &&
                    thp->as_prp != pcpu_array[get_cpunum()].pc_as_prp) {
                    /* Kernel is already locked so we don't need SWITCH_AS */
                    vm_ops->as(thp->as_prp,
                               &pcpu_array[get_cpunum()].pc_as_prp);
                }
                if (cop->flags & COF_NETCON) {
                    /* Note: have to adjust srcmsglen before stuffing rcvinfo!
                     */
                    sender->args.ms.srcmsglen = remote_srcmsglen;
                }
            }

            /* We can use a fast inline version as we know the thread does not
             * have an unblock pending
             */
            STUFF_RCVINFO(sender, cop, &rep);

            /* RUSH: Adjust msglen in better fashion... */
            if (thp->args.ms.srcmsglen < rep.msglen) {
                rep.msglen = thp->args.ms.srcmsglen;
            }
            if ((xferstat =
                     fastmsg_copy_rcvinfo(thp, thp->args.ri.info, &rep))) {
                goto rcv_fault;
            }
        }

        SETKSTATUS(thp, (sender->tid << 16) | cop->scoid);

        /* Unlink receive thread from the receive queue. Queue mutation under
         * chp->lock; thp's MSG_DELIVERY was held since the peek up top to keep
         * force_ready / other peekers off it. Clear the claim only AFTER
         * LINKPRIL_REM completes so a force_ready that wins the chp->lock race
         * after our release sees thp no longer on the queue.
         */
        kspinlock_lock(&chp->lock);
        LINKPRIL_REM(thp);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
        kspinlock_unlock(&chp->lock);

        sender->args.ms.server = thp;
        thp->client = sender;

        /* Set WAIT_PENDING on the sender BEFORE the state = STATE_REPLY →
         * LINKPRIL_BEG(reply_queue) transition. Cleared with a hardware fence
         * after LINKPRIL_BEG below. Pairs with the spin-wait in ksys_msg_replyv
         * post-lookup. This bridge prevents a concurrent replyv from finding
         * the sender state == REPLY but not yet on reply_queue (which would
         * corrupt the pril list on LINKPRIL_REM).
         */
        ITF_SET(sender, RTMUX_ITF_WAIT_PENDING);

        /* Check fast path conditions - no timeouts, no remote IPC, no sporadic.
         * We can inline the block_and_ready()
         */
        if ((sender->timeout_flags == 0) && (thp->timeout_flags == 0) &&
            !(cop->flags & COF_NETCON) &&
            !(chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) &&
            !IS_SCHED_SS(sender)) {
            /* By default the receiver runs with message driven priority. */
            thp->real_priority = thp->priority = sender->priority;
            thp->dpp = sender->dpp;
            CRITICAL_INHERIT(thp, sender);

            sender->state =
                STATE_REPLY; /* Must be set before calling block_and_ready() */
            snap_time(&sender->timestamp_last_block, 0);
            _TRACE_TH_EMIT_STATE(sender, REPLY);
#if defined(INLINE_BLOCKANDREADY)

            /* This is an inline version of block an ready We can use this for
             * non-SMP (no runmask). This also works for AP as we inherit the
             * partition
             */
            thp->next.thread = NULL;
            thp->prev.thread = NULL;
            /* thp->restart = NULL; */

            /* Direct sender->receiver switch: bill the sender (still this
             * hart's active thread) up to the timestamp just taken above.
             */
            sched_account_current(sender->timestamp_last_block);
            set_active_thread(get_cpunum(), thp);
            thp->state = STATE_RUNNING;

            /* @@@ Hmm. This inline version of block_and_ready() may cause a
             * small critical-accounting inaccuracy. Update runcpu to this CPU.
             * A stale runcpu would break unready's displaced-old-active
             * discrimination: a thread receiving a message via this inline path
             * runs on this CPU, so a later unready that compares thp->runcpu
             * against the current CPU must see them agree.
             */
            thp->runcpu = get_cpunum();
            _TRACE_TH_EMIT_STATE(thp, RUNNING);
#else
            block_and_ready(thp);
#endif
        } else {
            if ((chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
                /* By default the receiver runs with message driven priority. */
                thp->real_priority = thp->priority = sender->priority;
                thp->dpp = sender->dpp;
                CRITICAL_INHERIT(thp, sender);
            }
            sender->state =
                STATE_REPLY; /* Must be set before calling block_and_ready() */
            _TRACE_TH_EMIT_STATE(sender, REPLY);

            if (cop->flags & COF_NETCON) {
                SETKSTATUS(act, 1);
                if ((sender->flags & RTMUX_FLG_THR_BUFF_MSG) == 0) {
                    /* #### Note: use remote_srcmsglen saved above before we
                     * switch as
                     */
                    sender->args.ms.srcmsglen = remote_srcmsglen;
                }

                SETKSTATUS(thp, (sender->args.ms.rparts << 16) | cop->scoid);
                ready(thp);
            } else {
                block_and_ready(thp);
            }

            if (thp->timeout_flags & RTMUX_TIMEOUT_REPLY) {
                /* arm the timeout for reply block */
                timeout_start(thp);
            }
        }

        /* Block the active thread and ready the receiver thread */
        sender->blocked_on = cop;

        /* Link the now reply blocked sending thread in the reply queue Queue
         * mutation under chp->lock. No ->destroying recheck here (unlike the
         * send-block path): the common wake for a reply-blocked sender is the
         * server's ksys_msg_replyv via rcvid, independent of this channel. chp
         * is valid across this link (caller's RCU-sched read section), but that
         * section ends once the sender context-switches away, so in the narrow
         * window where a channel_destroy links this sender after its reply
         * drain has passed, chp can be freed with the sender still linked. That
         * stale reply_queue linkage is made memory-safe by force_ready's
         * STATE_REPLY->RECEIVE unlink, which skips LINKPRIL_REM when the owning
         * channel is gone (dispatch.c). Residual is liveness only.
         */
        kspinlock_lock(&chp->lock);
        LINKPRIL_BEG(chp->reply_queue, sender, thread_t);
        kspinlock_unlock(&chp->lock);
        *heldp = NULL; /* the reply-side drop releases it */

        /* Hardware SMP fence pairs with the spin-wait + fence in
         * ksys_msg_replyv (and any other waker that observes sender's
         * STATE_REPLY). The fence ensures the state / blocked_on /
         * on-reply_queue writes above are visible to other CPUs before
         * WAIT_PENDING goes back to 0.
         */
        cpu_smp_mb();
        ITF_CLR(sender, RTMUX_ITF_WAIT_PENDING);

        /* A reply timeout whose deadline passed during the block was captured
         * (timeout_start_impl defers while WAIT_PENDING is set, since inline
         * force_ready would spin on our own bit). Deliver it now that the
         * sender is fully linked.
         */
        timeout_deferred_fire();

        return ENOERROR;
    }

    /* No-one waiting for a msg If a normal thread
     *     Block the active thread
     *     Link the now send blocked thread into the reply queue
     * If a remote IPC thread send
     *     Link the passed vthread into the reply queue
     * Boost the servers priority to the clients if needed.
     */
    if (th_flags == 0) {
        sender->args.ms.smsg = kap->smsg;
        sender->args.ms.sparts = kap->sparts;
        /* FUTURE: Make copy of send IOVs */
    } else {
        atomic_or(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
    }


    if (IMTO(sender, STATE_SEND)) {
        atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
        return ETIMEDOUT;
    }


    /* Set WAIT_PENDING on the sender BEFORE the state = STATE_SEND →
     * pril_add(send_queue) transition. Cleared with a fence after pril_add
     * below. Pairs with the WAIT_PENDING spin in dispatch.c's force_ready
     * STATE_SEND case and any other waker walking chp->send_queue. Without
     * this, a concurrent force_ready on the sender while its state is set but
     * it is not yet on send_queue would call LINKPRIL_REM through a stale prev
     * pointer and corrupt the pril list.
     */
    ITF_SET(sender, RTMUX_ITF_WAIT_PENDING);

    /* Incoming remote IPC send. We use vtid passed in kap->rparts and
     * _vtid_info passed in kap->rmsg
     */
    if (cop->flags & COF_NETCON) {
        if (sender->flags & RTMUX_FLG_THR_BUFF_MSG) {
            SETKSTATUS(act, 1);
        } else {
            /* Return zero telling the remote IPC service we still need the send
             * data. A _NOTIFICATION_CODE_NET_ACK will be sent later when the
             * receive completes.
             */
            sender->args.ms.srcmsglen = remote_srcmsglen;
            SETKSTATUS(act, 0);
        }
        sender->state = STATE_SEND;
        snap_time(&sender->timestamp_last_block, 0);
        _TRACE_TH_EMIT_STATE(sender, SEND);
        sender->blocked_on = cop;
        /* Queue mutation under chp->lock */
        kspinlock_lock(&chp->lock);
        if (chp->destroying || cop->channel != chp) {
            kspinlock_unlock(&chp->lock);
            ITF_CLR(sender, RTMUX_ITF_WAIT_PENDING);
            atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
            sender->blocked_on = NULL;
            sender->state = STATE_STOPPED;
            return ESRCH;
        }
        pril_add(&chp->send_queue, sender);
        kspinlock_unlock(&chp->lock);
    } else {
        /* Don't allow any msg_send's to processes that are dying. Only have to
         * check here because of code in nano_signal.c
         * - check the comment where we turn on the RTMUX_FLG_PROC_COREDUMP
         * flag.
         */
        if (chp->process->flags &
            (RTMUX_FLG_PROC_TERMING | RTMUX_FLG_PROC_ZOMBIE |
             RTMUX_FLG_PROC_COREDUMP)) {
            ITF_CLR(sender, RTMUX_ITF_WAIT_PENDING);
            return ENXIO;
        }

        /* Re-check receive_queue under the SAME chp->lock window as pril_add.
         * Between our early peek and now, a receiver may have arrived, scanned
         * send_queue (still empty), and parked itself on receive_queue. Without
         * this re-check both sides end up blocked with no waker.
         *
         * The re-check + pril_add must be atomic under one slock acquisition;
         * dropping and re-acquiring would leave the same TOCTOU open. unready()
         * is moved to AFTER pril_add (still outside the slock window — locking
         * convention keeps CHANNEL > SCHED). Receivers that pick our entry off
         * send_queue spin on RTMUX_ITF_WAIT_PENDING (see the post-claim spin in
         * ksys_msg_receivev) until the unready settles, then observe a fully
         * coherent sender.
         *
         * blocked_on is set AFTER unready: unready→block() clears blocked_on to
         * NULL for non-STOPPED states. Setting it before unready would be
         * silently wiped; receivers waiting on WAIT_PENDING would then read a
         * NULL blocked_on. The WAIT_PENDING bridge keeps both fields (state,
         * blocked_on) invisible to wakers until the bit clears.
         */
        kspinlock_lock(&chp->lock);
        if (chp->destroying || cop->channel != chp) {
            kspinlock_unlock(&chp->lock);
            ITF_CLR(sender, RTMUX_ITF_WAIT_PENDING);
            atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
            return ESRCH;
        }
        {
            thread_t *recv_recheck = chp->receive_queue;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            while (recv_recheck &&
                   (recv_recheck->internal_flags & RTMUX_ITF_MSG_DELIVERY))
                recv_recheck = recv_recheck->next.thread;
#endif
            if (recv_recheck &&
                !(recv_recheck->internal_flags & RTMUX_ITF_RCVNOTIFICATION)) {
                /* Receiver showed up. Abandon the send-block path and
                 * KCALL_RESTART so the kcall re-runs and takes the matching
                 * path. Safe: sender hasn't been unready'd yet, state is still
                 * RUNNING.
                 */
                kspinlock_unlock(&chp->lock);
                ITF_CLR(sender, RTMUX_ITF_WAIT_PENDING);
                atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
                KCALL_RESTART(act);
                return ENOERROR;
            }
            pril_add(&chp->send_queue, sender);
        }
        kspinlock_unlock(&chp->lock);

        /* Now actually take sender off the runnable queue. In the window
         * between pril_add and unready, sender is on send_queue with state
         * still RUNNING; receivers that pick it up spin on WAIT_PENDING below
         * before reading state. Can't use block(), because 'sender' might not
         * actually be the actives[get_cpunum()] anymore...
         */
        unready(sender, STATE_SEND);

        /* Set blocked_on AFTER unready (block() inside unready cleared it to
         * NULL). WAIT_PENDING spinners haven't woken yet, so the read of
         * blocked_on after they exit the spin sees this assignment.
         */
        sender->blocked_on = cop;
    }

    *heldp = NULL; /* the reply-side drop releases it */

    /* Hardware fence then clear WAIT_PENDING. All sender writes (state,
     * blocked_on, on send_queue) are now visible to any waker spinning on the
     * bit before they observe its 1→0 transition.
     */
    cpu_smp_mb();
    ITF_CLR(sender, RTMUX_ITF_WAIT_PENDING);

    /* To prevent priority inversion, boost all threads in the server
     *
     * Raise the priority of any thread that last used this channel to at least
     * the sender's priority, but do not inherit critical-event state.
     */
    if ((chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
        int i;

        for (i = 0; i < chp->process->threads.nentries; ++i) {
            if (VECP(thp, &chp->process->threads, i) &&
                thp->last_chid == chp->chid) {
                int may_run = may_thread_run(thp);
                if (thp->priority < sender->priority) {
                    adjust_priority(thp, sender->priority,
                                    may_run ? thp->dpp : sender->dpp, 1);
                    thp->real_priority = thp->priority;
                } else {
                    if (!may_run) {
                        /* server threads are higher prio, but have no budget.
                         * So inherit budget only
                         */
                        adjust_priority(thp, thp->priority, sender->dpp, 1);
                    }
                }
            }
        }
    }

    /* Deliver a send timeout that expired while arming (see the REPLY path). */
    timeout_deferred_fire();

    return ENOERROR;

send_fault:
    atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)

    /* Clear MSG_DELIVERY claim acquired at the receive_queue peek; we never
     * reached LINKPRIL_REM, but thp is still valid (the fault was on the sender
     * side) and remains on the queue.
     */
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
    return EFAULT;

rcv_fault:
    atomic_clr(&sender->flags, RTMUX_FLG_THR_BUFF_MSG);
    kcall_err(thp, EFAULT);

    /* LINKPRIL_REM under chp->lock; clear MSG_DELIVERY claim INSIDE the lock
     * (after LINKPRIL_REM) so the same race window that bites the success path
     * can't bite here either.
     */
    kspinlock_lock(&chp->lock);
    LINKPRIL_REM(thp);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
    kspinlock_unlock(&chp->lock);
    ready(thp);

    /* Restart the kernel call - same behavior as receive path */
    KCALL_RESTART(act);

    return ENOERROR;
}


/* msg_sendv kcall: sendv_body() with the sender's connection reference released
 * on every path that did not publish the sender.
 */
int kdecl ksys_msg_sendv(thread_t *act, struct kcall_args_msg_sendv *kap)
{
    connect_t *held = NULL;
    int r = sendv_body(act, kap, &held);

    /* A reap request is ignored: freeing synchronizes RCU and this kcall still
     * holds its read section. That needs the fd closed, the channel destroyed
     * and the send failing all at once; the soul then stays until exit.
     */
    if (held)
        (void) connect_client_drop(held, act->real_priority);
    return r;
}

/* Send a notification on a connection (msg_send_notification kcall).
 *
 * Checks send permission, then either delivers the notification directly to a
 * waiting receiver in the right address space (short-circuit) or enqueues it
 * via notification_deliver. Parameters:
 *   act : the calling (active) thread
 *   kap : kcall args (coid, priority, notification code, notification value)
 * Return value: EOK on success; an errno on failure.
 */
int kdecl ksys_msg_sendnotification(thread_t *act,
                                    struct kcall_args_msg_sendnotification *kap)
{
    connect_t *cop;
    channel_t *chp;
    unsigned prio;
    unsigned deliver_prio;
    int status;
    thread_t *thp;

    /* Channel-queue exclusion stays under chp->lock; the MSG_DELIVERY claim
     * used by sendv is not needed here because notification delivery doesn't
     * reuse the receiver's reply path.
     *
     * Open a connobj_rcu read section before lookup_connect so the chp (=
     * cop->channel) we lock/queue on below cannot be freed by a concurrent
     * _channel_destroy (which rcu_sched_synchronize()s before object_free).
     * Mirrors ksys_msg_sendv; lookup_rcvid_release closes the nested guard at
     * trap exit.
     */
    lookup_rcvid_open();

    if (!(cop = lookup_connect(kap->coid)) || cop->type != TYPE_CONNECTION ||
        !(chp = cop->channel)) {
        return EBADF;
    }
    if ((__atomic_load_n(&cop->rights, __ATOMIC_RELAXED) &
         RTMUX_CAP_ENDPOINT_SEND) == 0)
        return EBADF;

    prio = (kap->priority == -1) ? act->priority
                                 : ((unsigned) kap->priority & 0xffU);
    deliver_prio = prio;
    if (kap->priority != -1 &&
        ((unsigned) kap->priority & RTMUX_NOTIFICATION_REPLACE) != 0)
        deliver_prio |= _NOTIFICATION_PRIO_REPLACE;

    /* Notification over the remote IPC service. */
    if (chp == remote_ipc.chp) {
        _TRACE_COMM_EMIT_SNOTIFICATION(act, cop, cop->scoid, prio);
        status = remote_ipc_notify(act, cop, (unsigned) kap->coid, prio,
                                   kap->code, kap->value);
        return status;
    }

    /* If we have a receive thread on the channel, and there is no other pending
     * notification (think notification prio inversion), we can take the
     * shortcut of delivering directly.
     */
    _TRACE_COMM_EMIT_SNOTIFICATION(act, cop, cop->scoid | RTMUX_SIDE_CHANNEL,
                                   prio);

    /* Short-circuit probe (receive_queue + send_queue) under chp->lock;
     * WAIT_PENDING spin-wait on the picked thp outside the lock.
     */
    {
        int short_circuit;
        kspinlock_lock(&chp->lock);

        /* Same ->destroying guard as the send paths: a concurrent
         * channel_destroy drains send_queue after setting ->destroying under
         * chp->lock. Without this recheck notification_deliver() below could
         * notification_add() a fresh soul onto a just-drained channel, leaking
         * it onto the chp that is about to be object_free'd. The standalone
         * ChannelDestroy case sets no process-exit flag, so this bit is the
         * only thing that catches it.
         */
        if (chp->destroying || cop->channel != chp) {
            kspinlock_unlock(&chp->lock);
            return ESRCH;
        }
        thp = chp->receive_queue;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        while ((thp) && (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY)) {
            thp = thp->next.thread;
        }
#endif
        short_circuit = (thp) &&
                        (thp->as_prp == pcpu_array[get_cpunum()].pc_as_prp) &&
                        !pril_first(&chp->send_queue);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)

        /* Claim the chosen thp before dropping chp->lock — without it a
         * concurrent sender on another CPU picks the same receiver and
         * double-unlinks it at LINKPRIL_REM. Mirrors notification_deliver().
         */
        if (short_circuit) {
            ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
        }
#endif
        kspinlock_unlock(&chp->lock);

        if (thp) {
            while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
                __asm__ volatile("" ::: "memory");
            cpu_smp_mb();
        }
        if (short_circuit) {
            /* Receive thread waiting with right as -- short-circuit
             * notification delivery
             */

            ITF_CLR(thp, RTMUX_ITF_RCVNOTIFICATION);
            thp->restart = NULL;

            /* Yes. Transfer the data. */
            status =
                xfernotification(thp, thp->args.ri.rmsg, thp->args.ri.rparts,
                                 (uint32_t) kap->code, (uintptr_t) kap->value,
                                 (int32_t) (cop->scoid | RTMUX_SIDE_CHANNEL));

            /* If the receiver asked for a msg_info, then stuff it right now
             * since we're in the right as from the test above
             */
            if (status == 0 && thp->args.ri.info) {
                struct _msg_info rep;

                /* We can use a fast inline version as we know the thread does
                 * not have an unblock pending STUFF_RCVINFO(act, cop,
                 * thp->args.ri.info);
                 */
                get_rcvinfo(act, -1, cop, &rep);
                status = fastmsg_copy_rcvinfo(thp, thp->args.ri.info, &rep);
            }


            SETKSTATUS(thp, 0);

            if (status) {
                kcall_err(thp, EFAULT);
            }

            /* Unlink receive thread from the receive queue and ready it. Queue
             * mutation under chp->lock
             */
            kspinlock_lock(&chp->lock);
            LINKPRIL_REM(thp);
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            atomic_clr((volatile unsigned *) &thp->internal_flags,
                       RTMUX_ITF_MSG_DELIVERY);
#endif
            kspinlock_unlock(&chp->lock);

            /* By default the receiver runs with message driven priority. */
            if (thp->priority != prio &&
                (chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
                adjust_priority(thp, prio, thp->process->default_dpp, 1);
                thp->real_priority = thp->priority;
            } else if (thp->dpp != thp->process->default_dpp) {
                adjust_priority(thp, thp->priority, thp->process->default_dpp,
                                1);
            }

            /* Note - since this is a user-delivered notification, it cannot
             * have the VTID flag on @@Add assert
             */
            ready(thp);

            return status ? EFAULT : EOK;

        } else {
            /* Express "use the non-critical heap for any notification
             * allocation" via the per-CPU soul_alloc_override slot — each CPU's
             * intent lives in its own slot, with no shared state mutated. See
             * lock.h.
             */
            {
                unsigned cpu = get_cpunum();
                pcpu_array[cpu].pc_soul_alloc_override =
                    SOUL_OVERRIDE_NONCRITICAL;
                status = notification_deliver(
                    chp, deliver_prio, kap->code, (uintptr_t) kap->value,
                    (intptr_t) (cop->scoid | RTMUX_SIDE_CHANNEL), 0);
                pcpu_array[cpu].pc_soul_alloc_override = SOUL_OVERRIDE_NONE;
            }

            if (status) {
                return status;
            }
        }
    } /* close short_circuit scope */

    return EOK;
}

/* Receive a message or notification from a channel (msg_receivev /
 * msg_receive_notificationv kcall).
 *
 * Validates the receive iovs, then either consumes a queued notification, picks
 * up a send-blocked sender (transferring its message and reply-blocking it),
 * bounces a timed-out sender, or blocks the caller on the channel's receive
 * queue when nothing is pending. Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (chid, receive iov rmsg/rparts, info buffer)
 * Return value: EOK / ENOERROR per the path taken; an errno on failure.
 */
int kdecl ksys_msg_receivev(thread_t *act, struct kcall_args_msg_receivev *kap)
{
    channel_t *chp;
    connect_t *cop;
    thread_t *thp;
    thread_t **owner;
    int tid;
    int notification_only = KTYPE(act) == __KSYS_MSG_RECEIVENOTIFICATIONV;
    unsigned chid;
    unsigned tls_flags;
    vector_t *chvec;

    /* Channel-queue exclusion stays under chp->lock; sender-thread claim under
     * MSG_DELIVERY (probe-and-claim).
     */

    chid =
        (unsigned) (act->last_chid = kap->chid); /* Used for priority boost */
    chvec = &act->process->chancons;

    /* Open a connobj_rcu read section so the receiver's own chp (looked up
     * below) can't be freed by a concurrent channel destroy during the
     * chp->lock window. The trap-exit lookup_rcvid_release closes it; RTMux's
     * block() only marks the blocked state (the real switch is at kernel_exit,
     * after the release), so this is safe across a receive block.
     */
    lookup_rcvid_open();

    /* Take the appropriate vector slock for the vector_lookup: prp->vec_slock
     * for the per-process case, global_channel_slock for the global directory.
     * Both are PROCVEC class, distinct instances. Held only across the lookup
     * itself — once chp is in hand, chp->lock takes over for the receive_queue
     * / send_queue mutations below. This closes the cross-CPU race against
     * ksys_channel_destroy's vector_rem on the same global_channel_vector.
     */
    {
        kspinlock_t *lookup_lock;
        if (chid & RTMUX_GLOBAL_CHANNEL) {
            chid &= ~RTMUX_GLOBAL_CHANNEL;
            chvec = &global_channel_vector;
            lookup_lock = &global_channel_slock;
        } else {
            lookup_lock = &act->process->vec_slock;
        }
        kspinlock_lock(lookup_lock);
        chp = vector_lookup(chvec, chid);
        kspinlock_unlock(lookup_lock);
    }
    if (!chp || chp->type != TYPE_CHANNEL) {
        return ESRCH;
    }

    if (kap->info) {
        WR_VERIFY_PTR(act, kap->info, sizeof(*kap->info));

        /* NOTE: Make sure the receive info pointer is valid. Note that we need
         * some extra checks in the mainline when filling in the rcvinfo (this
         * is no longer done in specret).
         *
         * Note: we don't probe the whole buffer, rather touch start and end,
         * which is faster and sufficient
         */
        WR_PROBE_INT(act, kap->info, 1);
        WR_PROBE_INT(act, &kap->info->endpoint_cap,
                     sizeof(kap->info->endpoint_cap) / sizeof(int));
    }

    if (chp->flags & RTMUX_FLG_CHF_GLOBAL) {
        /* Global channels not supported in RTMux */
        return ENOSYS;
    }

    /* Validate incoming IOVs and calculate receive length */
    if (kap->rparts >= 0) {
        size_t len = 0;
        size_t len_last = 0;
        iov_t *iov = kap->rmsg;
        int rparts = kap->rparts;

        if (kap->rparts != 0) {
            if (!WITHIN_BOUNDARY((uintptr_t) iov, (uintptr_t) (&iov[rparts]),
                                 act->process->boundary_addr)) {
                return EFAULT;
            }
        }

        /* Calculate receive length -- even if not requested, we use it for
         * msginfo Do boundary check
         */
        while (rparts) {
            uintptr_t base, last;

            len += GETIOVLEN(iov);
            if (len < len_last) {
                /* overflow. excessively long user iov_t, possibly overlayed.
                 * pr62575
                 */
                return EOVERFLOW;
            }
            len_last = len;
            base = (uintptr_t) GETIOVBASE(iov);
            last = base + GETIOVLEN(iov) - 1;
            if (((base > last) ||
                 !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) &&
                (GETIOVLEN(iov) != 0)) {
                return EFAULT;
            }
            ++iov;
            --rparts;
        }
        act->args.ms.srcmsglen = (uint32_t) len;
    } else {
        /* Single part -- validate receive address */
        uintptr_t base, last;
        base = (uintptr_t) kap->rmsg;
        last = base + (unsigned) PARTS_TO_LEN(kap->rparts) - 1;
        if ((base > last) ||
            !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) {
            /* We know length is non-zero from test above */
            return EFAULT;
        }
        act->args.ms.srcmsglen = (uint32_t) PARTS_TO_LEN(kap->rparts);
    }


restart:
    /* Probe + claim atomic under one chp->lock window.
     *
     * The single slock acquisition does:
     *   - pril_first(send_queue)
     *   - skip entries with RTMUX_ITF_MSG_DELIVERY set
     *     (already claimed by another receiver)
     *   - for RECEIVENOTIFICATIONV: skip non-notification entries (also
     * re-skipping claimed)
     *   - if a notification matches: snapshot notification fields into locals
     * and notification_remove (the notification pointer is invalid after the
     * drop)
     *   - if a thread sender matches and is NOT marked IMTO: claim it
     *     via START_SMP_XFER (sets MSG_DELIVERY).  IMTO senders are
     *     left in queue unclaimed; the IMTO branch outside the slock
     *     calls force_ready on them.
     *
     * After the drop, no other receiver will pick up our notification (it's
     * removed) or our claimed sender (MSG_DELIVERY blocks them at the skip-loop
     * above), so the heavy xfermsg / xfernotification work runs lock-free
     * without racing the queue.
     *
     * force_ready is NOT callable under chp->lock: force_ready takes chp->lock
     * itself for STATE_RECEIVE / REPLY / SEND. Hence the IMTO bounce is done
     * outside the window, with thp left visible (unclaimed) so force_ready's
     * queue walk finds it.
     */
    int notification_chosen = 0;
    int notification_code = 0;
    uintptr_t notification_value = 0;
    intptr_t notification_id = 0;
    uint8_t notification_priority = 0;
    thread_t *vnotification_origin = NULL;
    int imto_bounce = 0;

restart_recv:
    notification_chosen = 0;
    imto_bounce = 0;
    vnotification_origin = NULL;
    kspinlock_lock(&chp->lock);
    thp = receive_candidate(chp, notification_only);
    if (thp) {
        unsigned type = TYPE_MASK(thp->type);
        if (type == TYPE_NOTIFICATION || type == TYPE_VNOTIFICATION) {
            notification_t *pup = (notification_t *) (void *) thp;
            notification_chosen = 1;
            notification_code = pup->code;
            notification_value = pup->value;
            notification_id = (intptr_t) pup->id;
            notification_priority = pup->priority;
            if (type == TYPE_VNOTIFICATION)
                vnotification_origin = (thread_t *) pup->id;

            /* notification_remove may free pup; locals above are the survivors
             * of the slock drop.
             */
            notification_remove(chp->process, &chp->send_queue, pup);
        } else if (IMTO(thp, STATE_REPLY)) {
            /* Don't claim — leave for force_ready outside the lock. */
            imto_bounce = 1;
        } else {
            /* Thread sender — atomic claim via MSG_DELIVERY. */
            START_SMP_XFER(act, thp);
        }
    }
    kspinlock_unlock(&chp->lock);

    /* Once we have claimed a thread sender via MSG_DELIVERY, wait for its
     * WAIT_PENDING bridge bit to clear before reading its state / blocked_on /
     * xfer fields. The sender's path (ksys_msg_sendv no-receiver branch)
     * pril_adds BEFORE unreadying, so there is a brief window where thp is on
     * send_queue with state still RUNNING — we must wait it out or the
     * post-xfer state transition (REPLY vs NET_REPLY) routes wrong.
     * Notification and IMTO entries don't carry a transitioning sender, so the
     * spin only applies to the thread-sender claim case.
     */
    if (thp && !notification_chosen && !imto_bounce) {
        while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
            __asm__ volatile("" ::: "memory");
        cpu_smp_mb();
    }

    if (notification_chosen) {
        int xferstat;

        act->restart = NULL;
        xferstat = xfernotification(
            act, kap->rmsg, kap->rparts, (uint32_t) notification_code,
            notification_value, (int32_t) notification_id);

        if (xferstat == 0 && vnotification_origin && kap->info) {
            struct _msg_info rep;

            get_rcvinfo(vnotification_origin, -1,
                        vnotification_origin->blocked_on, &rep);
            xferstat = fastmsg_copy_rcvinfo(act, kap->info, &rep);
        }

        act->timeout_flags = 0;

        /* By default the receiver runs with message driven priority. RUSH: Fix
         * for partition inheritance
         */
        if (act->priority != notification_priority &&
            (chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
            adjust_priority(act, notification_priority,
                            act->process->default_dpp, 1);
            act->real_priority = act->priority;
        } else if (act->dpp != act->process->default_dpp) {
            adjust_priority(act, act->priority, act->process->default_dpp, 1);
        }

        if ((thp = act->client) != 0) {
            /* need to clear client's server field */
            act->client = 0;
            thp->args.ms.server = 0;
        }

        if (xferstat) {
            return EFAULT;
        }
        _TRACE_COMM_IPC_RET(act);

        return EOK;
    }

    if (imto_bounce) {
        force_ready(thp, ETIMEDOUT);
        KSYS_PREEMPT(act, ENOERROR); /* if needed, returns ENOERROR */
        goto restart;
    }

    if (thp) {
        fastmsg_native_cap_tx_t captx;
        int capstat = EOK;
        int cap_patched = 0;
        int xferstat;

        captx.hdr = NULL;
        captx.nxfers = 0;

        /* thp has been claimed (MSG_DELIVERY set) under channel_slock above;
         * from here on it's exclusively ours.
         */

        /* thp's MSG_DELIVERY was set under chp->lock above; START_SMP_XFER
         * below is idempotent for thp. The matching END_SMP_XFER runs in the
         * final chp->lock window after the pril_rem, keeping thp claimed for
         * the whole send_queue -> reply_queue transition so a concurrent
         * receiver cannot pick thp off send_queue between our xfer and our
         * pril_rem.
         */
        if (thp->flags & RTMUX_FLG_THR_BUFF_MSG) {
            capstat = fastmsg_native_cap_install_buffer(
                thp, act, thp->args.msbuff.buff, thp->args.msbuff.msglen, false,
                &captx);
            if (capstat == EOK)
                xferstat = xfer_cpy_diov(act, kap->rmsg, thp->args.msbuff.buff,
                                         kap->rparts, thp->args.msbuff.msglen);
            else
                xferstat = 0;
        } else {
            act->args.ri.rmsg = kap->rmsg;
            act->args.ri.rparts = kap->rparts;

            capstat = fastmsg_native_cap_install_long(thp, act, false, &captx,
                                                      &cap_patched);
            if (capstat == EOK) {
                xferstat = xfermsg(
                    act, thp, cap_patched ? sizeof(struct _io_native_hdr) : 0,
                    cap_patched ? sizeof(struct _io_native_hdr) : 0);
            } else if (capstat & (XFER_SRC_FAULT | XFER_DST_FAULT)) {
                xferstat = capstat;
                capstat = EOK;
            } else {
                xferstat = 0;
            }


#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
                /* Sender was force_ready'd by another CPU during our xfer
                 * (e.g., channel destroy). Drop our claim, complete the
                 * deferred force_ready, restart.
                 */
                kspinlock_lock(&chp->lock);
                END_SMP_XFER(act, thp);
                ITF_CLR(thp, RTMUX_ITF_MSG_FORCE_RDY);
                kspinlock_unlock(&chp->lock);
                force_ready(thp, MSG_FORCE_ERROR(thp));
                KCALL_RESTART(act);
                act->restart = 0;
                return ENOERROR;
            }
            if (act->flags &
                (RTMUX_FLG_THR_SIG_ACTIVE | RTMUX_FLG_THR_CANCELSELF)) {
                kspinlock_lock(&chp->lock);
                END_SMP_XFER(act, thp);
                kspinlock_unlock(&chp->lock);
                KCALL_RESTART(act);
                act->restart = 0;
                return ENOERROR;
            }
#endif
        }

        if (capstat != EOK) {
            kspinlock_lock(&chp->lock);
            END_SMP_XFER(act, thp);
            kspinlock_unlock(&chp->lock);
            force_ready(thp, (unsigned) capstat);
            KSYS_PREEMPT(act, ENOERROR);
            goto restart;
        }

        if (xferstat) {
            fastmsg_native_cap_rollback(act, &captx);

            /* Drop our claim — both fault-recovery branches below leave thp on
             * send_queue (the send-fault branch will force_ready it; the
             * dst-fault branch returns EFAULT to the receiver while the sender
             * stays send-blocked).
             */
            kspinlock_lock(&chp->lock);
            END_SMP_XFER(act, thp);
            kspinlock_unlock(&chp->lock);

            /* Only a send fault will unblock the sender. */
            if (xferstat & XFER_SRC_FAULT) {
                /* Let sender know it faulted and restart receive. */
                force_ready(thp, EFAULT);
                KSYS_PREEMPT(act, ENOERROR);
                goto restart;
            }

            if ((thp = act->client) != 0) {
                /* need to clear client's server field */
                act->client = 0;
                thp->args.ms.server = 0;
            }

            /* Let receiver and sender know reason for fault. */
            act->timeout_flags = 0;
            return EFAULT;
        }
        if (cap_patched)
            thp->args.ms.msglen += (uint32_t) sizeof(struct _io_native_hdr);

        if (TYPE_MASK(thp->type) == TYPE_VTHREAD) {
            tid = thp->args.ri.rparts;
        } else {
            tid =
                (int) thp->tid; /* tid fits in int, used in rcvid calculation */
        }
        cop = thp->blocked_on;
        if (thp->args.ms.srcmsglen == ~0U) {
            /* This should never occur with the new code */
            crash();
            /* NOTREACHED */
            thp->args.ms.srcmsglen = thp->args.ms.msglen;
        }

        /* If the receive specified an info buffer stuff it as well.
         * thp->args.ms.msglen was set by xfermsg
         */
        if (kap->info) {
            /*  get_rcvinfo(thp, -1, cop, kap->info); */
            STUFF_RCVINFO(thp, cop, kap->info);
            if (thp->flags & RTMUX_FLG_THR_BUFF_MSG) {
                if (kap->info->msglen > act->args.ms.srcmsglen)
                    kap->info->msglen = act->args.ms.srcmsglen;
            }
        }

        _TRACE_COMM_IPC_RET(act);
        act->timeout_flags = 0;
        act->restart = NULL;

        /* Because RTMUX_FLG_THR_RCVINFO and RTMUX_FLG_THR_SHORT_MSG will not be
         * set, set this to NULL
         */
        thp->restart = NULL;

        if (act->client != 0) {
            /* need to clear client's server field */
            act->client->args.ms.server = 0;
        }
        thp->args.ms.server = act;
        act->client = thp;

        /* pril_rem off send_queue, state transition, LINKPRIL_BEG onto
         * reply_queue, AND END_SMP_XFER (clear MSG_DELIVERY claim +
         * act->blocked_on) — all under one chp->lock window.
         *
         * The unclaim happens *after* pril_rem so the entire send_queue →
         * reply_queue transition is atomic for any concurrent receiver: while
         * thp is on send_queue, it carries MSG_DELIVERY (skipped by other
         * probes); once pril_rem'd, it's no longer on send_queue, so other
         * probes simply don't find it; clearing MSG_DELIVERY at that point is
         * safe.
         */
        kspinlock_lock(&chp->lock);
        pril_rem(&chp->send_queue, thp);
        if (thp->state == STATE_SEND) {
            thp->state = STATE_REPLY;
            snap_time(&thp->timestamp_last_block, 0);
            _TRACE_TH_EMIT_STATE(thp, REPLY);
            SETKSTATUS(act, (tid << 16) | cop->scoid);
        } else {
            thp->state = STATE_NET_REPLY;
            _TRACE_TH_EMIT_STATE(thp, NET_REPLY);
            SETKSTATUS(act, -((tid << 16) | cop->scoid));
        }
        LINKPRIL_BEG(chp->reply_queue, thp, thread_t);
        END_SMP_XFER(act, thp);
        kspinlock_unlock(&chp->lock);

        /* The sender blocked in SEND before any receiver was waiting, so its
         * reply timeout was not armed (timeout_start only arms for the state
         * the thread blocks in). It is reply-blocked now: arm it, or a timed
         * wait whose server was busy at send time waits for the reply forever.
         * A sender that already armed a SEND|REPLY timeout keeps its timer
         * (TIMEOUT_ACTIVE guard). thp is on reply_queue with WAIT_PENDING clear
         * and no lock is held, so an expired deadline may fire inline.
         */
        if (thp->timeout_flags & RTMUX_TIMEOUT_REPLY)
            timeout_start(thp);

        /* By default the receiver runs with message driven priority. RUSH: Fix
         * for partition inheritance
         */
        if ((act->priority != thp->priority || act->dpp != thp->dpp) &&
            (chp->flags & RTMUX_FLG_CHF_FIXED_PRIORITY) == 0) {
            CRITICAL_INHERIT(act, thp);
            adjust_priority(act, thp->priority, thp->dpp, 1);
            if (act->real_priority != act->priority)
                act->real_priority = act->priority;
        } else {
            CRITICAL_CLEAR(act);
        }

        return ENOERROR;
    }

    /* No-one waiting for a msg — block on chp->receive_queue.
     *
     * Canonical WAIT_PENDING waiter pattern. Sequence:
     *
     *   1. early-exit checks (act->client cleanup, IMTO, PENDCAN)
     *   2. set WAIT_PENDING on act (bridge bit visible to wakers)
     *   3. under chp->lock: walk receive_queue, LINKPRIL_BEG act
     *   4. drop chp->lock
     *   5. unready(act, STATE_RECEIVE) — sets state, clears active,
     *      and (importantly) zeroes act->blocked_on inside block()
     *   6. restore act->blocked_on / args.ri after unready, since
     *      block() just zeroed it
     *   7. compiler barrier
     *   8. clear WAIT_PENDING — wakers spinning on the bit can now
     *      observe a fully-coherent (state, blocked_on, on-list)
     *      waiter
     *
     * Wakers (ksys_msg_sendv, force_ready STATE_RECEIVE / REPLY) spin on
     * RTMUX_ITF_WAIT_PENDING before pril_rem'ing the receiver, so they never
     * see act in a half-blocked state.
     */
    tls_flags = TLS_FLAGS_SAFE(act);
    _TRACE_COMM_IPC_RET(act);

    if ((thp = act->client) != 0) {
        /* need to clear client's server field */
        act->client = 0;
        thp->args.ms.server = 0;
    }

    if (IMTO(act, STATE_RECEIVE)) {
        return ETIMEDOUT;
    }

    if (PENDCAN(tls_flags)) {
        SETKIP_FUNC(act, act->process->canstub);
        return ENOERROR;
    }

    /* Step 2: set WAIT_PENDING before LINKPRIL_BEG so concurrent wakers that
     * walk the queue spin until our unready completes.
     */
    ITF_SET(act, RTMUX_ITF_WAIT_PENDING);

    /* Step 3: receive_queue insertion under chp->lock. RECEIVENOTIFICATIONV
     * waiters go after non-notification-only ones so ksys_msg_sendv only needs
     * to inspect the head.
     *
     * The first probe dropped chp->lock before reaching here. In that window a
     * sender could have pushed onto chp->send_queue via ksys_msg_sendv's
     * pril_add. If we blindly pril_add act onto chp->receive_queue while a
     * sender is waiting, both threads end up blocked on opposite queues with
     * nobody to wake them — a lost wakeup. Re-probe send_queue under the lock;
     * if anything is there (sender thread, notification, or otherwise), drop
     * the lock and goto the top of the kernel extension to reuse the existing
     * claim/delivery path. act is still STATE_RUNNING (no unready yet), so the
     * restart is safe.
     */
    kspinlock_lock(&chp->lock);

    /* A channel destroy sets ->destroying under chp->lock before it drains
     * receive_queue. Linking act after that drain passed would leave it queued
     * on a channel that is about to be freed, with nobody left to wake it (the
     * drain loops, or a later teardown, then find a stale head). Mirror the
     * send paths' recheck.
     */
    if (chp->destroying) {
        kspinlock_unlock(&chp->lock);
        ITF_CLR(act, (RTMUX_ITF_WAIT_PENDING | RTMUX_ITF_RCVNOTIFICATION));
        return ESRCH;
    }
    {
        /* Match the first probe: ordinary senders cannot wake a
         * notification-only receive, and notification payload is not a claim.
         */
        thread_t *thp_recheck = receive_candidate(chp, notification_only);
        if (thp_recheck) {
            kspinlock_unlock(&chp->lock);

            /* Clear transition flags before retrying the first probe. */
            ITF_CLR(act, (RTMUX_ITF_WAIT_PENDING | RTMUX_ITF_RCVNOTIFICATION));
            goto restart_recv;
        }
    }
    owner = &chp->receive_queue;
    if (notification_only) {
        ITF_SET(act, RTMUX_ITF_RCVNOTIFICATION);
        for (;;) {
            thp = *owner;
            if (!thp)
                break;
            if (thp->internal_flags & RTMUX_ITF_RCVNOTIFICATION)
                break;
            owner = &thp->next.thread;
        }
    }
    LINKPRIL_BEG(*owner, act, thread_t);
    kspinlock_unlock(&chp->lock);

    /* Step 5: unready (sched_slock-self-locking). block() inside zeroes
     * act->blocked_on; the WAIT_PENDING bridge covers the window where act is
     * on receive_queue with state==RUNNING.
     */
    unready(act, STATE_RECEIVE);

    /* End inheritance of partition and critical state. Must be after block() so
     * we micro-bill against the partition we were running in before resetting
     * to the original. PR26990.
     */
    act->dpp = act->orig_dpp;
    CRITICAL_CLEAR(act);

    /* Step 6: restore act->blocked_on / args.ri after unready (block NULL'd
     * blocked_on).
     */
    act->blocked_on = chp;
    act->args.ri.rmsg = kap->rmsg;
    act->args.ri.rparts = kap->rparts;
    act->args.ri.info = kap->info;

    /* Step 7+8: hardware SMP fence (rw,rw), then clear WAIT_PENDING. The fence
     * pairs with the waker's "cpu_smp_mb()" after its WAIT_PENDING spin —
     * without it, on a weakly-ordered SMP target (RISC-V is RVWMO), the sender
     * on another CPU could see the cleared WAIT_PENDING bit before the
     * preceding writes to blocked_on / args.ri / state become visible, and then
     * act on a partially-coherent receiver. A compiler barrier is not enough
     * here because the issue is hardware reordering, not just compiler
     * reordering.
     */
    cpu_smp_mb();
    ITF_CLR(act, RTMUX_ITF_WAIT_PENDING);

    /* Deliver a receive timeout that expired while arming. */
    timeout_deferred_fire();

    /* Hardware fence AFTER the WAIT_PENDING clear to force the clear out of
     * this hart's store buffer before the kcall returns. The fence BEFORE the
     * clear ordered the preceding writes (state, blocked_on, args.ri) ahead of
     * the clear — but the clear itself could sit in this hart's store buffer
     * from the spec's point of view (RVWMO). A sender on another hart spinning
     * on WAIT_PENDING reads through cache coherence; without a sender-side
     * fence inside the spin, the spinner may keep seeing the pre-clear value
     * until the store drains.
     */
    cpu_smp_mb();
    return ENOERROR;
}


/* Reply to a client and unblock it (msg_replyv kcall).
 *
 * Validates the reply iovs, transfers the reply data to the reply-blocked
 * client thread named by kap->rcvid, then unlinks it from the reply queue and
 * readies it with the supplied status. The client is claimed via MSG_DELIVERY
 * so a concurrent error/force_ready cannot race the unlink. Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (rcvid, reply iov smsg/sparts, status)
 * Return value: EOK on success; an errno on fault/lookup failure.
 */
int kdecl ksys_msg_replyv(thread_t *act, struct kcall_args_msg_replyv *kap)
{
    connect_t *cop;
    thread_t *thp;
    channel_t *chp; /* = cop->channel, for chp->lock */
    int xferstat, status;
    unsigned flags = 0;

    /* Reply-queue exclusion stays under chp->lock; receiver claim under
     * MSG_DELIVERY is preserved.
     */

    /* Future: Think about an inline version of this for speed */
    if ((cop = lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) ==
        NULL) {
        return ENOERROR;
    }
    chp = cop->channel; /* channel for the per-channel chp->lock */

    /* Spin-wait if thp is mid-transition into STATE_REPLY + on reply_queue.
     * Pairs with ksys_msg_sendv's WAIT_PENDING set+clear around its
     * sender->state = STATE_REPLY → LINKPRIL_BEG window. The bridge prevents us
     * from acting on a thp whose state has been set but whose reply_queue
     * insertion has not landed (which would make our LINKPRIL_REM dereference a
     * stale prev pointer). The trailing fence pairs with the writer's
     * smp_mb-before-clear.
     */
    while (thp->internal_flags & RTMUX_ITF_WAIT_PENDING)
        __asm__ volatile("" ::: "memory");
    cpu_smp_mb();

    /* Claim thp via RTMUX_ITF_MSG_DELIVERY under chp->lock after re-validating
     * state. Mirrors the claim in sendv: once the bit is set, force_ready on
     * thp defers (via force_ready_msgxfer in dispatch.c's STATE_REPLY case) so
     * the LINKPRIL_REM(thp) at the success/error tails of this kcall can no
     * longer race against a concurrent ksys_connect_detach's EBADF wakeup
     * cascade. The bit is cleared INSIDE chp->lock after each LINKPRIL_REM
     * (success path + xfer_err path). Every early-return path between here and
     * LINKPRIL_REM must clear it — see the explicit clears below.
     */
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    kspinlock_lock(&chp->lock);
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        kspinlock_unlock(&chp->lock);
        return ESRCH;
    }
    ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    kspinlock_unlock(&chp->lock);
#else

    /* Make sure thread is replied blocked on a channel owned by this process.
     */
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        return ESRCH;
    }
#endif

    /* See comment in ksys_msg_error about channel_destroy handling. This is a
     * placeholder to make sure that the remote IPC service keeps calling
     * msg_error rather than msg_reply.
     */
    CRASHCHECK(_TRACE_GETSYSCALL(thp->syscall) == __KSYS_CHANNEL_DESTROY);

    /* Verify that the message has been fully received, and that the receiving
     * thread has completed any specialret() processing that needs to be done.
     */
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
        ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
        return ESRCH;
    }

    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;

        /* NULL: client's blocked_on was cleared by a concurrent teardown
         * (force_ready on a client whose process is exiting) -> ESRCH. Without
         * it the deref below load-faults near NULL.
         */
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
            return ESRCH;
        }
    }

    if (thp->internal_flags & RTMUX_ITF_UNBLOCK_QUEUED) {
        remove_unblock(thp, cop, kap->rcvid);
        KSYS_PREEMPT(act, ENOERROR);
    }

    atomic_clr(&thp->flags, (RTMUX_FLG_THR_BUFF_MSG | RTMUX_FLG_THR_SHORT_MSG));

    if (kap->sparts != 0) {
        fastmsg_native_cap_tx_t captx;

        /* Only a client that opted in (RTMUX_ITF_NATIVE_REPLY, set by the
         * client before its send, so ordered before this reply by the
         * rendezvous) gets its reply parsed as a native cap header. Any other
         * reply is opaque data: its bytes cannot name capabilities.
         */
        bool native_reply = (thp->internal_flags & RTMUX_ITF_NATIVE_REPLY) != 0;

        captx.hdr = NULL;
        captx.nxfers = 0;
        /* Reply IOVs */
        if (kap->sparts < 0) {
            /* Single part -- do the boundary check and copy if short message */
            uintptr_t base, last;
            int len;

            base = (uintptr_t) kap->smsg;
            len = (int) PARTS_TO_LEN(kap->sparts);
            last = base + (unsigned) len - 1;
            if ((base > last) ||
                !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) {
                /* We know length is non-zero from test above */
                xferstat = XFER_SRC_FAULT;
                goto xfer_err;
            }
            if (len <= sizeof(thp->args.msbuff.buff)) {
                if ((xferstat = xfer_memcpy(
                         thp->args.msbuff.buff, (char *) base,
                         thp->args.msbuff.msglen = (uint32_t) len))) {
                    goto xfer_err;
                }
                flags = RTMUX_FLG_THR_BUFF_MSG;
            }
        } else if (kap->sparts == 1) {
            /* Single iov_t -- do the boundary check and copy if short message
             */
            uintptr_t base, last, len;

            base = (uintptr_t) GETIOVBASE(kap->smsg);
            len = GETIOVLEN(kap->smsg);
            last = base + len - 1;
            if (((base > last) ||
                 !WITHIN_BOUNDARY(base, last, act->process->boundary_addr)) &&
                (len != 0)) {
                xferstat = XFER_SRC_FAULT;
                goto xfer_err;
            }
            if (len <= sizeof(thp->args.msbuff.buff)) {
                if ((xferstat =
                         xfer_memcpy(thp->args.msbuff.buff, (char *) base,
                                     thp->args.ms.msglen = (uint32_t) len))) {
                    goto xfer_err;
                }
                flags = RTMUX_FLG_THR_BUFF_MSG;
            }
        } else {
            /* Multi iov_t case */
            size_t len = 0;
            size_t len_last = 0;
            iov_t *iov = kap->smsg;
            int sparts = kap->sparts;

            /* Calculate reply length -- even if not requested, it is almost
             * free Also do boundary check
             */
            while (sparts) {
                uintptr_t base, last;

                len += GETIOVLEN(iov);
                if (len < len_last) {
                    /* overflow. excessively long user iov_t, possibly
                     * overlayed. pr62575
                     */
                    xferstat = XFER_SRC_FAULT;
                    goto xfer_err;
                }
                len_last = len;
                base = (uintptr_t) GETIOVBASE(iov);
                last = base + GETIOVLEN(iov) - 1;
                if (((base > last) ||
                     !WITHIN_BOUNDARY(base, last,
                                      act->process->boundary_addr)) &&
                    (GETIOVLEN(iov) != 0)) {
                    xferstat = XFER_SRC_FAULT;
                    goto xfer_err;
                }
                ++iov;
                --sparts;

                /* TODO: cache iov_t copy for deferred send (only needed if no
                 * receiver) if(niov < _NUM_CACHED_SEND_IOV) {
                 * act->args.ms.siov[niov] = *iov; }
                 */
            }
            if (len <= sizeof(thp->args.msbuff.buff)) {
                size_t pos = 0;
                iov = kap->smsg;
                sparts = kap->sparts;

                /* Multi-iov_t incoming message that is short Optimization --
                 * need memcpy_siov for efficiency
                 */
                while (sparts) {
                    if ((xferstat =
                             xfer_memcpy(&thp->args.msbuff.buff[pos],
                                         GETIOVBASE(iov), GETIOVLEN(iov)))) {
                        goto xfer_err;
                    }

                    pos += GETIOVLEN(iov);
                    iov++;
                    sparts--;
                }
                thp->args.ms.msglen = (uint32_t) len;
                flags = RTMUX_FLG_THR_BUFF_MSG;
            }
        }

        /* Up-front work is now done. There are a few things set:
         * - incoming reply IOVs are verified for boundary
         * - if short message, it has been copied into thp's short buffer
         * - flags is either zero (long message) or RTMUX_FLG_THR_BUFF_MSG if
         * short
         */

        /* This is a long message */
        if (flags == 0) {
            int cap_patched = 0;

            act->args.ms.smsg = kap->smsg;
            act->args.ms.sparts = kap->sparts;
            xferstat = native_reply ? fastmsg_native_cap_install_long(
                                          act, thp, true, &captx, &cap_patched)
                                    : 0;
            if (xferstat) {
                if ((xferstat & (XFER_SRC_FAULT | XFER_DST_FAULT)) == 0)
                    xferstat = XFER_SRC_FAULT;
                goto xfer_err;
            }
            START_SMP_XFER(act, thp);
            /* Yes. Transfer the data. */
            xferstat = xfermsg(thp, act,
                               cap_patched ? sizeof(struct _io_native_hdr) : 0,
                               cap_patched ? sizeof(struct _io_native_hdr) : 0);
            /* Keep thp's claim until the success tail unlinks it. */
            END_SMP_XFER_SELF(act);

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
            if (thp->internal_flags & RTMUX_ITF_MSG_FORCE_RDY) {
                _TRACE_COMM_EMIT_REPLY(thp, cop, thp->tid + 1);

                /* it could be channel destroy: drop our claim and the flag
                 * together under chp->lock, then complete the abort.
                 */
                kspinlock_lock(&chp->lock);
                ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY | RTMUX_ITF_MSG_FORCE_RDY);
                kspinlock_unlock(&chp->lock);
                force_ready(thp, MSG_FORCE_ERROR(thp));
                KCALL_RESTART(act);
                act->restart = 0;
                return ENOERROR;
            }
#endif
            if (xferstat)
                fastmsg_native_cap_rollback(thp, &captx);
        } else {
            /* Short message. We only do something if there is no target as --
             * reply to proc case. Else, this is handled in specret.
             */
            xferstat = native_reply ? fastmsg_native_cap_install_buffer(
                                          act, thp, thp->args.msbuff.buff,
                                          thp->args.msbuff.msglen, true, &captx)
                                    : 0;
            if (xferstat) {
                xferstat = XFER_SRC_FAULT;
                goto xfer_err;
            }
            if (!thp->as_prp) {
                xferstat =
                    xfer_cpy_diov(thp, thp->args.ri.rmsg, thp->args.msbuff.buff,
                                  thp->args.ri.rparts, thp->args.msbuff.msglen);
            } else {
                xferstat = 0;
                thp->blocked_on = thp;
                atomic_or(&thp->flags,
                          (RTMUX_FLG_THR_BUFF_MSG | RTMUX_FLG_THR_SHORT_MSG));
            }
            if (xferstat) {
                fastmsg_native_cap_rollback(thp, &captx);
                xferstat = XFER_SRC_FAULT;
                goto xfer_err;
            }
        }
        _TRACE_COMM_EMIT_REPLY(thp, cop, thp->tid + 1);
    } else { /* Zero-length reply -- common case */
        xferstat = 0;
        _TRACE_COMM_EMIT_REPLY(thp, cop, thp->tid + 1);
    }


    atomic_clr(&thp->flags, RTMUX_FLG_THR_UNBLOCK_REQ);

    if (thp->args.ms.server != 0) {
        thp->args.ms.server->client = 0;
        thp->args.ms.server = 0;
    }

    if (xferstat)
        goto xfer_err;

    thp->restart = act->restart = NULL;

    /* msg_replyv's status is the value returned by the client's msg_send(). For
     * I/O replies that carry payload, I/O services conventionally return the
     * number of bytes produced. Do not let a stale or otherwise oversized
     * positive status report more bytes than the client made room for;
     * proc_read's combined lseek+read path uses that value to advance through
     * its destination buffer.
     */
    if (kap->status > 0 && kap->sparts != 0 && thp->args.ms.rparts != 0 &&
        (uint32_t) kap->status > thp->args.ms.dstmsglen) {
        kap->status = (int) thp->args.ms.dstmsglen;
    }

    /* LINKPRIL_REM under chp->lock; clear the MSG_DELIVERY claim INSIDE the
     * lock so a concurrent force_ready that wins the chp->lock race after our
     * release sees thp no longer on reply_queue and skips its own LINKPRIL_REM.
     */
    kspinlock_lock(&chp->lock);
    LINKPRIL_REM(thp);

    /* Completed under the lock: a force_ready (ConnectDetach's EBADF sweep)
     * that wins the lock after our release sees blocked_on != cop and leaves
     * thp, and its link, alone; thp stays STATE_REPLY until ready() below. (The
     * short-reply copy above already set blocked_on to thp itself, which the
     * client's special return needs.)
     */
    if (thp->blocked_on == cop)
        thp->blocked_on = NULL;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    /* The reply completes the exchange, so it supersedes an abort a channel
     * destroy deferred to us; drop that flag with the claim, in the same lock
     * window, so it cannot linger on a healthy client.
     */
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY | RTMUX_ITF_MSG_FORCE_RDY);
#endif
    kspinlock_unlock(&chp->lock);
    if (connect_links_drop(cop)) {
        connect_detach(cop, thp->priority);
    }

    /* Publish the reply status into thp's trap frame BEFORE making it runnable.
     * On SMP, ready(thp) can dispatch the client on another hart immediately;
     * if SETKSTATUS ran after, the client could return from MsgSend reading a
     * stale status, and SETKSTATUS would be writing the trap frame of a thread
     * already executing on another hart (live-context corruption).
     */
    SETKSTATUS(thp, (unsigned) kap->status);
    ready(thp);

    return EOK;

xfer_err:

    /* Fault -- determine if this is sender or replyer. If replier faulted let
     * him know and abort the operation and wakup the sender with an error.
     */
    status = (xferstat & XFER_SRC_FAULT) ? EFAULT : EFAULT;
    kcall_err(thp, status);

    /* Same LINKPRIL_REM + claim/flag clear under chp->lock as the success path.
     */
    kspinlock_lock(&chp->lock);
    LINKPRIL_REM(thp);
    /* Complete the exchange under the lock; see ksys_msg_replyv. */
    if (thp->blocked_on == cop)
        thp->blocked_on = NULL;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY | RTMUX_ITF_MSG_FORCE_RDY);
#endif
    kspinlock_unlock(&chp->lock);
    atomic_clr(&thp->flags, (RTMUX_FLG_THR_BUFF_MSG | RTMUX_FLG_THR_SHORT_MSG));

    if (connect_links_drop(cop)) {
        connect_detach(cop, thp->priority);
    }
    ready(thp);

    return status;
}

/* Reply to a client with an error and unblock it (msg_error kcall).
 *
 * Wakes the reply-blocked client thread named by kap->rcvid with the supplied
 * errno (or restarts its kcall for ERESTART), unlinks it from the reply queue,
 * and detaches the connection if its last link goes away. The client is claimed
 * via MSG_DELIVERY to keep concurrent reply/force_ready paths off it.
 * Parameters:
 *   act : the calling (active) thread (the server)
 *   kap : kcall args (rcvid, err)
 * Return value: EOK on success; ESRCH if the rcvid no longer names a
 *               reply-blocked client.
 */
int kdecl ksys_msg_error(thread_t *act, struct kcall_args_msg_error *kap)
{
    connect_t *cop;
    thread_t *thp;
    channel_t *chp; /* = cop->channel, for chp->lock */

    /* Reply-queue exclusion stays under chp->lock; receiver claim under
     * MSG_DELIVERY is preserved.
     */

    if ((cop = lookup_rcvid((kcall_args_t *) (void *) kap, kap->rcvid, &thp)) ==
        NULL) {
        return ENOERROR;
    }
    chp = cop->channel; /* channel for the per-channel chp->lock */

#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)

    /* Atomic state-check + MSG_DELIVERY claim under chp->lock. Mirrors the
     * claim pattern in ksys_msg_replyv. Without this, the window between a
     * plain state check and the eventual LINKPRIL_REM lets a concurrent
     * ksys_msg_replyv (or a force_ready cascade from ksys_connect_detach's
     * EBADF path) wake thp first; we then arrive at ready(thp) with thp already
     * STATE_READY — caught by the READY invariant in dispatch.c.
     *
     * If state has already flipped, OR another path is mid-claim on this thread
     * (replyv claimed MSG_DELIVERY), bail with ESRCH. The rcvid is effectively
     * gone from the perspective of this msg_error caller — it can either retry
     * or accept that the reply (or another error) won the race.
     *
     * force_ready's STATE_REPLY case checks MSG_DELIVERY and defers via
     * force_ready_msgxfer when set, so once we hold the claim concurrent wakers
     * stand off.
     */
    kspinlock_lock(&chp->lock);
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        kspinlock_unlock(&chp->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        kspinlock_unlock(&chp->lock);
        return ESRCH;
    }
    if (thp->internal_flags & RTMUX_ITF_MSG_DELIVERY) {
        /* Another path holds the claim on this thread. */
        kspinlock_unlock(&chp->lock);
        return ESRCH;
    }
    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
            kspinlock_unlock(&chp->lock);
            return ESRCH;
        }
    }
    ITF_SET(thp, RTMUX_ITF_MSG_DELIVERY);
    kspinlock_unlock(&chp->lock);
#else

    /* Make sure thread is replied blocked on a channel owned by this process.
     */
    if (thp->state != STATE_REPLY && thp->state != STATE_NET_REPLY) {
        return ESRCH;
    }

    /* Verify that the message has been fully received, and that the receiving
     * thread has completed any specialret() processing that needs to be done.
     */
    if (thp->internal_flags & RTMUX_ITF_SPECRET_PENDING) {
        return ESRCH;
    }

    if (thp->blocked_on != cop) {
        connect_t *cop1 = cop;
        cop = thp->blocked_on;

        /* NULL: client's blocked_on was cleared by a concurrent teardown
         * (force_ready on a client whose process is exiting) -> ESRCH. Without
         * it the deref below load-faults near NULL.
         */
        if (!cop || (cop->flags & COF_VCONNECT) == 0 ||
            cop->un.lcl.cop != cop1) {
            return ESRCH;
        }
    }
#endif

    if (thp->internal_flags & RTMUX_ITF_UNBLOCK_QUEUED) {
        remove_unblock(thp, cop, kap->rcvid);
    }


    if (thp->args.ms.server != 0) {
        thp->args.ms.server->client = 0;
        thp->args.ms.server = 0;
    }

    atomic_clr(&thp->flags,
               (RTMUX_FLG_THR_BUFF_MSG | RTMUX_FLG_THR_UNBLOCK_REQ));
    if (kap->err == ERESTART) {
        CRASHCHECK(TYPE_MASK(thp->type) != TYPE_THREAD);
        SETKIP(thp, KIP(thp) - KSYS_ENTRY_SIZE);
    } else {
        if (_TRACE_GETSYSCALL(thp->syscall) == __KSYS_CHANNEL_DESTROY) {
            /* you may think we're crazy... but... ok, we ARE, but it's for the
             * best, really.
             *
             * The only way we could be doing a msg_error on a thread that is
             * really calling channel_destroy is if we were handling the
             * destruction of a channel with remote connections. The remote send
             * path is doing a msg_error to let us know the status of that
             * message. Unfortunately, the status field will overwrite the
             * syscall type field of the thread, which will mean that he will
             * restart his kernel call with the wrong kernel call number!
             *
             * Aiieeeeee!
             */
            if (kap->err != EOK) {
                LOGF_W("remote channel_destroy failed %d\n", kap->err);
            }
        } else {
            kcall_err(thp, kap->err);
        }
    }
    _TRACE_COMM_EMIT_ERROR(thp, cop, thp->tid + 1);

    /* LINKPRIL_REM + MSG_DELIVERY claim clear under chp->lock. Same atomicity
     * discipline as ksys_msg_replyv / ksys_msg_sendv: the claim that gated
     * other wakers off this thread is released INSIDE the chp->lock window,
     * after LINKPRIL_REM completes, so any concurrent path that wakes up
     * immediately after our release sees thp no longer on the queue and skips
     * its own LINKPRIL_REM.
     */
    kspinlock_lock(&chp->lock);
    LINKPRIL_REM(thp);
    /* Complete the exchange under the lock; see ksys_msg_replyv. */
    if (thp->blocked_on == cop)
        thp->blocked_on = NULL;
#if CONFIG_PROCESSORS_MAX > 1 && defined(CONFIG_SMP_MSGOPT)
    ITF_CLR(thp, RTMUX_ITF_MSG_DELIVERY);
#endif
    kspinlock_unlock(&chp->lock);
    if (connect_links_drop(cop)) {
        connect_detach(cop, thp->priority);
    }
    ready(thp);

    act->restart = NULL;
    return EOK;
}
