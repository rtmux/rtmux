/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * system logger service
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"

/* _IO_NOTIFY: the log has input once a record is queued. Output and out-of-band
 * data are never ready, so those arms wait until the open closes.
 */
int io_notify(ioserv_context_t *ctp, io_notify_t *msg, struct slog_ocb *ocb)
{
    struct slogdev *trp = ocb->dev;

    return ioserv_notify(ctp, msg, trp->notify,
                         trp->cnt ? _NOTIFY_COND_INPUT : 0, 0, NULL);
}
