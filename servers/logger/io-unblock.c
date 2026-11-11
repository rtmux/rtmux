/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"


int io_unblock(ioserv_context_t *ctp,
               io_notification_t *msg,
               struct slog_ocb *ocb)
{
    struct waiting *wap, *wap_prev;
    struct slogdev *trp;

    (void) msg;
    /* Remove from blocked readers. */
    trp = ocb->dev;
    wap_prev = (struct waiting *) &trp->waiting;
    for (; (wap = wap_prev->next); wap_prev = wap)
        if (wap->rcvid == ctp->rcvid) {
            wap_prev->next = wap->next;
            free(wap);
            return EINTR;
        }

    return IOSERV_NOREPLY;
}
