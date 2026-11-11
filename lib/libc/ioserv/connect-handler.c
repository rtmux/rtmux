/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <atomic.h>
#include <align.h>
#include <errno.h>
#include <pthread.h>
#include <stddef.h>
#include <sys/iomsg.h>
#include <sys/ioserv-private.h>
#include <sys/rtmux-core.h>
#include "internal.h"

static inline void *align_msg(void *base, size_t offset, size_t len)
{
    return (char *) base + lib_align_up_size(offset + len, RTMUX_MSG_ALIGN);
}

typedef int (*_ioserv_connect_func_t)(ioserv_context_t *ctp,
                                      ioserv_msg_t *msg,
                                      void *handle,
                                      void *extra);

int _ioserv_connect_handler(ioserv_context_t *ctp, ioserv_msg_t *msg)
{
    struct link *link;
    const ioserv_connect_ops_t *funcs;

    /* path_len is a client-supplied length. The bytes after the fixed
     * _io_connect header hold path_len path bytes and then any combine or extra
     * payload, so ctp->size, the combine sub-message pointer, and the extra
     * pointer are all derived from it. Reject a connect whose fixed header plus
     * claimed path does not fit in the bytes actually received: a forged
     * path_len larger than the message would otherwise push those derived
     * pointers past the receive buffer (the io handler offset guard truncates
     * the 64-bit displacement to int and lets it through). The arithmetic stays
     * in size_t so a 32-bit path_len cannot overflow it.
     */
    if (offsetof(struct _io_connect, path) + msg->open.connect.path_len >
        ctp->info.msglen) {
        return EBADMSG;
    }

    if (!(link = _ioserv_link_query((int) msg->open.connect.handle, 1))) {
        return ENOENT;
    }

    ctp->id = link->id;

    if (!(funcs = link->connect_funcs)) {
        _ioserv_link_return(link, 1);
        return IOSERV_DEFAULT;
    }

    /* msg_key_data verification goes here!!! */

    ctp->offset = 0;
    switch (msg->open.connect.subtype) {
    case _IO_CONNECT_COMBINE:
    case _IO_CONNECT_COMBINE_CLOSE: {
        int n;
        int rcvid;
        int subtype;
        struct binding *binding;
        ioserv_msg_t *combined;

        if (!funcs->open) {
            _ioserv_link_return(link, 1);
            return ENOSYS;
        }

        /* Fake up an open message */
        subtype = msg->open.connect.subtype;
        ctp->size = (int) (offsetof(struct _io_connect, path) +
                           msg->open.connect.path_len);

        pthread_setspecific(_ioserv_thread_key, link);
        n = funcs->open(ctp, (io_open_t *) &msg->open.connect, link->handle, 0);

        if ((link = pthread_getspecific(_ioserv_thread_key))) {
            pthread_setspecific(_ioserv_thread_key, NULL);
            _ioserv_link_return(link, 1);
        }

        if (n != EOK || ((unsigned) ctp->status & _IO_CONNECT_RET_FLAG)) {
            return n;
        }
        if ((binding = (struct binding *) _ioserv_handle(
                 &ctp->info, 0, _IOSERV_HANDLE_FIND_LOCK)) == (void *) -1) {
            return EBADF;
        }
        atomic_add(&binding->count, 1);

        if (msg->open.connect.extra_len >= sizeof(struct _io_combine)) {
            /* Call the combined message passing the opened OCB */
            ctp->status = 0;
            combined = (ioserv_msg_t *) align_msg(
                msg, offsetof(struct _io_connect, path),
                msg->open.connect.path_len);
            n = _ioserv_io_handler(ctp, combined, binding);
            /* _ioserv_io_handler unlocks so reacquire */
            if (_ioserv_handle(&ctp->info, 0, _IOSERV_HANDLE_FIND_LOCK) ==
                (void *) -1) {
                _ioserv_close_handler(ctp, binding);
                return EBADF;
            }
        }

        /* Finish the transaction calling the close message, but ignore its
         * return value
         */
        if (subtype == _IO_CONNECT_COMBINE_CLOSE &&
            _ioserv_handle(&ctp->info, 0, _IOSERV_HANDLE_FIND) != (void *) -1) {
            int status = ctp->status;

            rcvid = ctp->rcvid;
            ctp->rcvid = 0;
            msg->close.i.type = _IO_CLOSE;
            msg->close.i.combine_len = sizeof msg->close.i;
            (void) _ioserv_io_handler(ctp, msg, binding);
            ctp->rcvid = rcvid;
            ctp->status = status;
        } else
            _ioserv_handle(&ctp->info, 0, _IOSERV_HANDLE_UNLOCK);

        _ioserv_close_handler(ctp, binding);
        return n;
    }
    default:
        if ((unsigned) (msg->connect.subtype - _IO_CONNECT_OPEN) <
            funcs->nfuncs) {
            int status;
            _ioserv_connect_func_t func;

            if ((func = *(((_ioserv_connect_func_t *) (&funcs->open)) +
                          msg->connect.subtype - _IO_CONNECT_OPEN))) {
                void *extra = align_msg(msg, offsetof(struct _io_connect, path),
                                        msg->open.connect.path_len);

                ctp->size = (int) ctp->info.msglen;

                pthread_setspecific(_ioserv_thread_key, link);
                status = func(ctp, msg, link->handle, extra);

                if ((link = pthread_getspecific(_ioserv_thread_key))) {
                    pthread_setspecific(_ioserv_thread_key, NULL);
                    _ioserv_link_return(link, 1);
                }

                return status;
            }
        }
        _ioserv_link_return(link, 1);
        break;
    }
    return IOSERV_DEFAULT;
}
