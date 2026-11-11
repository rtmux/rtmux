/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/dispatch.h>
#include <sys/ioserv.h>
#include "internal.h"
#include "dispatch/state.h"

void _ioserv_handler(ioserv_context_t *ctp)
{
    int n;
    ioserv_msg_t *msg;
    dispatch_t *dpp;
    struct binding *binding;
    int (*func)(ioserv_context_t *ctp, ioserv_msg_t *msg, void *ocb);

    msg = ctp->msg;
    dpp = ctp->dpp;
    n = IOSERV_DEFAULT;
    ctp->status = 0;
    if (ctp->rcvid != -1) {
        if (ctp->rcvid == 0) {
            if (msg->type == _NOTIFICATION_TYPE &&
                msg->notification.subtype == _NOTIFICATION_SUBTYPE) {
                struct _notification notification;

                switch (msg->notification.code) {
                case _NOTIFICATION_CODE_DISCONNECT:
                    (void) _ioserv_disconnect_handler(ctp, msg,
                                                      msg->notification.scoid);
                    break;

                case _NOTIFICATION_CODE_UNBLOCK:
                    notification = msg->notification;
                    n = _ioserv_unblock_handler(
                        ctp, msg, msg->notification.value.sival_int);
                    if ((unsigned) n == IOSERV_DEFAULT) {
                        msg->notification = notification;
                        ctp->rcvid = 0;
                        if (dpp->other_func) {
                            n = dpp->other_func(ctp, msg);
                        }
                    }
                    break;

                default:
                    if (dpp->other_func) {
                        (void) dpp->other_func(ctp, msg);
                    }
                    break;
                }
                if ((unsigned) n == IOSERV_DEFAULT) {
                    return;
                }
            }
        } else {
            if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
                msg_error(ctp->rcvid, ENOTSUP);
                return;
            }
            ioserv_reply_counters_record_receive(0);
            switch (msg->type) {
            case _IO_CONNECT:
                n = _ioserv_connect_handler(ctp, msg);
                break;

            case _IO_DUP:
                n = _ioserv_dup_handler(ctp, msg);
                break;

            case _IO_MMAP:
                n = _ioserv_mmap_handler(ctp, msg);
                break;

            case _IO_OPENFD:
                n = _ioserv_openfd_handler(ctp, msg);
                break;

            case _IO_CLOSE:
                if ((binding = (struct binding *) _ioserv_handle(
                         &ctp->info, 0, _IOSERV_HANDLE_REMOVE_LOCK)) ==
                    (void *) -1) {
                    n = EBADF;
                    break;
                }
                if ((func = _ioserv_io_func(binding->funcs, _IO_CLOSE))) {
                    ctp->offset = 0;
                    ctp->size = (int) ctp->info.msglen;
                    ctp->id = binding->id;
                    n = func(ctp, msg, binding->ocb);
                } else {
                    n = EOK;
                }

                _ioserv_close_handler(ctp, binding);

                break;

            case _IO_NOTIFY:
                if (((unsigned) msg->notify.i.flags & _NOTIFY_COND_EXTEN) &&
                    (msg->notify.i.action &
                     _NOTIFY_ACTION_POLL)) { /* POLL or POLLARM */
                    n = _ioserv_notify_handler(ctp, msg);
                    break;
                }
                /* Fallthrough */

            default:
                if (msg->type >= _IO_BASE && msg->type <= _IO_MAX) {
                    if ((binding = (struct binding *) _ioserv_handle(
                             &ctp->info, 0, _IOSERV_HANDLE_FIND_LOCK)) ==
                        (void *) -1) {
                        n = EBADF;
                    } else {
                        n = _ioserv_io_handler(ctp, msg, binding);
                    }
                } else {
                    n = IOSERV_DEFAULT;
                }
                break;
            }
        }
    }

    if ((unsigned) n == IOSERV_DEFAULT && dpp->other_func) {
        n = dpp->other_func(ctp, msg);
    }

    switch ((unsigned) n) {
    case (unsigned) IOSERV_NOREPLY:
        break;

    case (unsigned) IOSERV_DEFAULT:
        n = ENOSYS;
        /* Fall through */
    default:
        ioserv_reply_counters_record(n > 0 ? n : EOK, 0);
        if (n <= 0) {
            (void) msg_replyv(ctp->rcvid, ctp->status, ctp->iov, -n);
        } else {
            msg_error(ctp->rcvid, n);
        }
    }
}
