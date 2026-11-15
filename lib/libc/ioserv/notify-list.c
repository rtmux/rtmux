/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * Readiness notification lists for _IO_NOTIFY handlers (poll and select).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioserv.h>
#include <sys/iomsg.h>
#include <sys/rtmux-core.h>
#include <sys/siginfo.h>

void ioserv_notify_init(ioserv_notify_t *nop)
{
    int i;

    for (i = 0; i < IOSERV_NOTIFY_COUNT; i++) {
        nop[i].cnt = INT_MAX;
        nop[i].list = NULL;
    }
}

/* The link to the caller's node on one condition list, keyed by client thread
 * and open; *result is NULL when it has none.
 */
static ioserv_notify_event_t **notify_find(const ioserv_context_t *ctp,
                                           ioserv_notify_t *nop)
{
    ioserv_notify_event_t **pp;

    for (pp = &nop->list; *pp; pp = &(*pp)->next)
        if ((*pp)->rcvid == ctp->rcvid && (*pp)->scoid == ctp->info.scoid &&
            (*pp)->coid == ctp->info.coid)
            break;
    return pp;
}

/* Recompute the lowest armed threshold on a list after a node is freed, so
 * ioserv_notify_armed() stops reporting a waiter that is gone.
 */
static void notify_recount(ioserv_notify_t *nop)
{
    const ioserv_notify_event_t *event;

    nop->cnt = INT_MAX;
    for (event = nop->list; event; event = event->next)
        if (event->cnt < nop->cnt)
            nop->cnt = event->cnt;
}

/* Unlink and free the node *pp points at. */
static void notify_drop(ioserv_notify_event_t **pp)
{
    ioserv_notify_event_t *event = *pp;

    *pp = event->next;
    free(event);
}

/* Reply with the satisfied conditions sat (in the client's form, ext) and the
 * output-only hup codes. Hangup and error are extended (POLL*) codes in
 * flags_exten: they do not fit the condition nibble, and a poll client takes
 * them as output-only revents, so a set hup also ends the wait. A condition arm
 * with nothing to report answers EAGAIN: armed, a notification follows.
 */
static int notify_reply(ioserv_context_t *ctp,
                        io_notify_t *msg,
                        int action,
                        unsigned sat,
                        unsigned ext,
                        unsigned hup)
{
    memset(&msg->o, 0, sizeof msg->o);
    msg->o.flags = sat ? sat | ext : 0;
    msg->o.flags_exten = hup;
    if (action == _NOTIFY_ACTION_CONDARM && !sat && !hup)
        return EAGAIN;
    return IOSERV_PTR(ctp, &msg->o, sizeof msg->o);
}

int ioserv_notify_ready(ioserv_context_t *ctp, io_notify_t *msg, unsigned hup)
{
    return notify_reply(
        ctp, msg, msg->i.action,
        msg->i.flags & (unsigned) (_NOTIFY_COND_INPUT | _NOTIFY_COND_OUTPUT),
        msg->i.flags & (unsigned) _NOTIFY_COND_EXTEN, hup);
}

int ioserv_notify(ioserv_context_t *ctp,
                  io_notify_t *msg,
                  ioserv_notify_t *nop,
                  unsigned trig,
                  unsigned hup,
                  const int *notifycnts)
{
    ioserv_notify_event_t *event;
    unsigned flags, ext;
    int i, action;

    /* Fold the extended POLL* codes into the condition nibble, so a service
     * passing either form of a condition matches a request in either form.
     */
    trig |= trig << 28;
    trig &= ~(unsigned) _NOTIFY_COND_EXTEN;
    trig |= trig >> 28;

    action = msg->i.action;
    flags = msg->i.flags;
    ext = flags & (unsigned) _NOTIFY_COND_EXTEN;
    flags &= ~(unsigned) _NOTIFY_COND_EXTEN;

    if (action != _NOTIFY_ACTION_TRANARM) {
        trig &= flags;
    } else {
        /* One client at a time may hold a transition arm on the input. */
        for (event = nop[IOSERV_NOTIFY_INPUT].list; event; event = event->next)
            if (event->scoid != ctp->info.scoid ||
                SIGEV_GET_TYPE(&msg->i.event) != SIGEV_NONE)
                return EBUSY;
        trig = 0;
    }

    flags |= flags >> 28;
    flags &= (1u << _NOTIFY_MAXCOND) - 1u;
    for (i = 0; i < IOSERV_NOTIFY_COUNT && flags; i++, flags >>= 1) {
        ioserv_notify_event_t **pp;

        if (!(flags & 1u))
            continue;
        pp = notify_find(ctp, &nop[i]);
        if (trig == 0 && action != _NOTIFY_ACTION_POLL &&
            SIGEV_GET_TYPE(&msg->i.event) != SIGEV_NONE) {
            if (!(event = *pp)) {
                if (!(event = malloc(sizeof *event)))
                    return ENOMEM;
                event->rcvid = ctp->rcvid;
                event->scoid = ctp->info.scoid;
                event->coid = ctp->info.coid;
                if (!nop[i].list)
                    nop[i].cnt = INT_MAX; /* a zeroed list holds no arm */
                event->next = nop[i].list;
                nop[i].list = event;
            }
            event->flags = ext;
            event->event = msg->i.event;
            event->cnt = action == _NOTIFY_ACTION_TRANARM ? 0
                         : notifycnts                     ? notifycnts[i]
                                                          : 1;
            if (event->cnt < nop[i].cnt)
                nop[i].cnt = event->cnt;
        } else if (*pp) {
            /* Ready now, the poll pass, or an arm with no event (the disarm
             * idiom): drop what an earlier pass left. Nothing is allocated
             * here, so this path cannot fail.
             */
            notify_drop(pp);
            notify_recount(&nop[i]);
        }
    }

    return notify_reply(ctp, msg, action, trig, ext, hup);
}

void ioserv_notify_trigger(ioserv_notify_t *nop, int cnt, int index)
{
    ioserv_notify_event_t **pp;

    if (!ioserv_notify_armed(nop, index, cnt))
        return;
    nop += index;
    nop->cnt = INT_MAX;
    for (pp = &nop->list; *pp;) {
        ioserv_notify_event_t *event = *pp;

        if (event->cnt <= cnt) {
            /* Tell the client which condition fired, in the form it asked. */
            if (SIGEV_GET_TYPE(&event->event) >= SIGEV_SIGNAL &&
                SIGEV_GET_TYPE(&event->event) <= SIGEV_NOTIFICATION &&
                event->event.sigev_code == SI_NOTIFY) {
                event->event.sigev_value.sival_int |=
                    (int) ((unsigned) _NOTIFY_COND_INPUT << index);
                if (event->flags & _NOTIFY_COND_EXTEN) {
                    event->event.sigev_value.sival_int |=
                        (int) _NOTIFY_COND_EXTEN;
                    event->event.sigev_code =
                        (short) -(int) (_NOTIFY_CONDE_RDNORM << index);
                }
            }
            /* One-shot: a poller re-arms on its next pass if it must wait. */
            (void) msg_deliver_event(event->rcvid, &event->event);
            notify_drop(pp);
            continue;
        }
        if (nop->cnt > event->cnt)
            nop->cnt = event->cnt;
        pp = &event->next;
    }
}

void ioserv_notify_remove(const ioserv_context_t *ctp, ioserv_notify_t *nop)
{
    int i;

    for (i = 0; i < IOSERV_NOTIFY_COUNT; i++) {
        ioserv_notify_event_t **pp = &nop[i].list;
        int dropped = 0;

        while (*pp) {
            if (!ctp || ((*pp)->scoid == ctp->info.scoid &&
                         (*pp)->coid == ctp->info.coid)) {
                notify_drop(pp);
                dropped = 1;
            } else {
                pp = &(*pp)->next;
            }
        }
        if (dropped)
            notify_recount(&nop[i]);
    }
}
