/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

static struct slog_notify_event *notify_lookup(int rcvid,
                                               struct slog_notify *notify)
{
    struct slog_notify_event *event;

    for (event = notify->list; event; event = event->next) {
        if (event->rcvid == rcvid)
            return event;
    }
    event = malloc(sizeof(*event));
    if (!event)
        return NULL;
    event->next = notify->list;
    notify->list = event;
    return event;
}

int io_notify(ioserv_context_t *ctp, io_notify_t *msg, struct slog_ocb *ocb)
{
    static const int notifycnts[SLOG_NOTIFY_COUNT] = {
        [SLOG_NOTIFY_INPUT] = 1,
        [SLOG_NOTIFY_OUTPUT] = 1,
        [SLOG_NOTIFY_OBAND] = 1,
    };
    struct slog_notify_event *event;
    unsigned trig;
    unsigned msgflags;
    unsigned ext;
    unsigned flags;
    struct slogdev *trp;
    int action, i;

    trig = 0;

    trp = ocb->dev;
    if (trp->cnt)
        trig |= _NOTIFY_COND_INPUT;

    trig |= trig << 28;
    trig &= ~(unsigned) _NOTIFY_COND_EXTEN;
    trig |= trig >> 28;

    action = msg->i.action;
    msgflags = msg->i.flags;
    ext = msgflags & (unsigned) _NOTIFY_COND_EXTEN;
    flags = msgflags & ~(unsigned) _NOTIFY_COND_EXTEN;
    if (action != _NOTIFY_ACTION_TRANARM) {
        trig &= flags;
    } else {
        for (event = trp->notify[SLOG_NOTIFY_INPUT].list; event;
             event = event->next) {
            if (SIGEV_GET_TYPE(&event->event) != SIGEV_NONE) {
                if (event->scoid != ctp->info.scoid ||
                    SIGEV_GET_TYPE(&msg->i.event) != SIGEV_NONE)
                    return EBUSY;
            }
        }
        trig = 0;
    }

    msgflags = flags | (flags >> 28);
    msgflags &= ((1u << _NOTIFY_MAXCOND) - 1u);
    for (i = 0; i < SLOG_NOTIFY_COUNT && msgflags; i++, msgflags >>= 1) {
        if ((msgflags & 1u) == 0)
            continue;
        event = notify_lookup(ctp->rcvid, &trp->notify[i]);
        if (!event)
            return ENOMEM;
        event->rcvid = ctp->rcvid;
        event->scoid = ctp->info.scoid;
        event->coid = ctp->info.coid;
        event->flags = ext;
        if (trig == 0 && action != _NOTIFY_ACTION_POLL) {
            event->event = msg->i.event;
            event->cnt = action == _NOTIFY_ACTION_TRANARM ? 0 : notifycnts[i];
            if (event->cnt < trp->notify[i].cnt)
                trp->notify[i].cnt = event->cnt;
        } else {
            SIGEV_NONE_INIT(&event->event);
            event->cnt = INT_MAX;
        }
    }

    msg->o.flags = trig;
    if (ext)
        msg->o.flags |= (uint32_t) _NOTIFY_COND_EXTEN;

    if (action & _NOTIFY_ACTION_POLL)
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
    if (trig || action != _NOTIFY_ACTION_CONDARM)
        return IOSERV_PTR(ctp, &msg->o, sizeof(msg->o));
    return EAGAIN;
}

void slog_notify_trigger(struct slogdev *trp, int index)
{
    struct slog_notify *notify;
    struct slog_notify_event *event;

    notify = &trp->notify[index];
    notify->cnt = INT_MAX;
    for (event = notify->list; event; event = event->next) {
        if (SIGEV_GET_TYPE(&event->event) == SIGEV_NONE)
            continue;
        if (event->cnt <= (int) trp->cnt) {
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
            (void) msg_deliver_event(event->rcvid, &event->event);
            event->event.sigev_notify = SIGEV_NONE;
            event->cnt = INT_MAX;
        }
        if (notify->cnt > event->cnt)
            notify->cnt = event->cnt;
    }
}

void slog_notify_remove(ioserv_context_t *ctp, struct slogdev *trp)
{
    struct slog_notify_event *prev, *event;
    int i;

    for (i = 0; i < SLOG_NOTIFY_COUNT; i++) {
        prev = (struct slog_notify_event *) &trp->notify[i].list;
        while ((event = prev->next)) {
            if (event->scoid == ctp->info.scoid &&
                event->coid == ctp->info.coid) {
                prev->next = event->next;
                free(event);
            } else {
                prev = event;
            }
        }
    }
}
