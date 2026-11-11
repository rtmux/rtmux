/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

static int wait_add(struct slogdev *trp, int rcvid, int priority);

int io_read(ioserv_context_t *ctp, io_read_t *msg, struct slog_ocb *ocb)
{
    int nonblock;
    int nparts;
    unsigned n;
    unsigned cnt;
    unsigned i;
    unsigned room;
    int *ptr, *ptrend;
    struct slogdev *trp;

    if ((ocb->ioflag & _IO_FLAG_RD) == 0)
        return EBADF;
    nonblock = slog_is_nonblock((int) ocb->ioflag, (int) msg->i.xtype);

    /* No special xtypes */
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE)
        return EINVAL;

    /* Reads must be at least _SLOG_HDRINTS (3) and a multiple of an int. */
    if ((n = (unsigned) (msg->i.nbytes / sizeof(int))) < _SLOG_HDRINTS)
        return EINVAL;
    if (msg->i.nbytes & (sizeof(int) - 1))
        return EINVAL;

    trp = ocb->dev;

    /* First time in after an open ptr will be NULL. */
    ptr = OCBGET(ocb);
    if (!ptr)
        OCBGET(ocb) = ptr = trp->get;

    /* If there are no events block. */
    if (ptr == trp->put) {
        if (nonblock)
            return EAGAIN;

        if (wait_add(trp, ctp->rcvid, ctp->info.priority) != 0)
            return EAGAIN;

        return IOSERV_NOREPLY;
    }

    /* Calculate the number of ints we can transfer. To keep the clients life
     * easy we never return a partial event.
     */
    for (cnt = 0, ptrend = ptr; ptrend != trp->put;) {
        i = (unsigned) _SLOG_GETCOUNT(*ptrend) + _SLOG_HDRINTS;
        if (i + cnt >= n)
            break;

        cnt += i;
        ptrend += i;
        if (ptrend >= trp->end)
            ptrend = trp->beg + (ptrend - trp->end);
    }

    /* If the users buffer was to small let him know. */
    if (cnt == 0)
        return E2BIG;

    /* Really used for debugging */
    if (Verbose >= 2)
        printf("Get %5ld %5u\n", ptr - trp->beg, cnt);

    /* How many ints can we get without wrapping. */
    room = (unsigned) (trp->end - ptr);
    if (cnt > room) {
        /* Wrap around and send data in 2 parts. */
        SETIOV(ctp->iov + 0, ptr, room * sizeof(int));
        SETIOV(ctp->iov + 1, trp->beg, (cnt - room) * sizeof(int));
        nparts = 2;

        ptr = &trp->beg[cnt - room];
    } else {
        /* No wrap around, send data in only 1 part. */
        SETIOV(ctp->iov + 0, ptr, cnt * sizeof(int));
        nparts = 1;

        if (room == cnt)
            ptr = trp->beg;
        else
            ptr += cnt;
    }

    /* Update position and access time. */
    OCBGET(ocb) = ptr;

    /* Reply with the data. */
    if (msg_replyv(ctp->rcvid, (int) (cnt * sizeof(int)), &ctp->iov[0],
                   nparts) == -1)
        return errno;

    return IOSERV_NOREPLY;
}

/* PR26878 Waiting requests are inserted in priority order. */
static int wait_add(struct slogdev *trp, int rcvid, int priority)
{
    struct waiting *new_wap;

    /* Get a new wait entry. */
    if (!(new_wap = malloc(sizeof(*new_wap))))
        return -1;

    /* Add the entry to the wait queue. */
    new_wap->rcvid = rcvid;
    new_wap->priority = priority;

    /* Put higher priority clients first */
    if (!trp->waiting || priority >= trp->waiting->priority) {
        new_wap->next = trp->waiting;
        trp->waiting = new_wap;
    } else {
        struct waiting *wap = trp->waiting;

        for (; wap->next && priority < wap->next->priority; wap = wap->next)
            ;

        new_wap->next = wap->next;
        wap->next = new_wap;
    }

    return 0;
}
