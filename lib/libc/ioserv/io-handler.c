/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <sys/rtmux-core.h>
#include <atomic.h>
#include <sys/ioserv-private.h>
#include "internal.h"

static void _ioserv_unlock(ioserv_context_t *ctp, struct binding *binding)
{
    _ioserv_func_t func;

    if ((func = _ioserv_io_func(binding->funcs, _IO_RSVD_UNLOCK_OCB))) {
        (void) func(ctp, 0, binding->ocb);
    }
}

int _ioserv_io_handler(ioserv_context_t *ctp,
                       ioserv_msg_t *msg,
                       struct binding *binding)
{
    int n;
    int locked;

    /* Although all threads who would get a reference to binding via same coid
     * are blocked, should still atomic this since another thread could get a
     * reference via duped coid.
     */
    atomic_add(&binding->count, 1);
    _ioserv_handle(&ctp->info, 0, _IOSERV_HANDLE_UNLOCK);

    locked = 0;
    ctp->id = binding->id;
    n = EBADMSG;
    for (;;) {
        int rcvid;
        int combine;
        _ioserv_func_t func;

        /* msg is advanced by a client-influenced combine length at the end of
         * the loop, so compute the sub-message displacement at full pointer
         * width. A narrowing (int) bounds check could wrap a past-the-end
         * pointer back into range; compare the wide value before trusting it.
         */
        long off = (char *) msg - (char *) ctp->msg;

        if (off < 0 || off >= (long) ctp->info.msglen) {
            break;
        }
        ctp->offset = (int) off;

        ctp->size = (int) (ctp->info.msglen - (unsigned) ctp->offset);
        rcvid = ctp->rcvid;
        combine = 0;
        if (msg->combine.combine_len & _IO_COMBINE_FLAG) {
            combine = msg->combine.combine_len & ~_IO_COMBINE_FLAG;

            /* combine_len is a client-supplied sub-message length. Cap it to
             * the bytes actually received (ctp->size still holds that
             * remainder) so a forged length cannot make a handler read past the
             * message into the context buffer. A well-formed combine
             * sub-message is never larger than the remainder, so this only
             * trims malformed input.
             */
            if (combine > ctp->size)
                combine = ctp->size;
            ctp->size = combine;
            ctp->rcvid = 0;
        }

        func = _ioserv_io_func(binding->funcs, msg->type);

        switch (msg->type) {
        case _IO_CLOSE:
            /* Pass in the binding we're working on as a hint. If it doesn't
             * match, it was already removed (and possible replaced) while we
             * were unlocked.
             */
            if (_ioserv_handle(&ctp->info, binding,
                               _IOSERV_HANDLE_REMOVE_LOCK) != (void *) -1) {
                if (locked) {
                    _ioserv_unlock(ctp, binding);
                    locked = 0;
                }
                n = func ? func(ctp, msg, binding->ocb) : EOK;
                atomic_sub(&binding->count, 1);
            }
            combine = 0;
            break;

        default:
            if (func) {
                _ioserv_func_t lock;

                if (!locked && (lock = _ioserv_io_func(binding->funcs,
                                                       _IO_RSVD_LOCK_OCB))) {
                    int rcvid2 = ctp->rcvid;

                    ctp->rcvid = rcvid;
                    if ((n = lock(ctp, 0, binding->ocb)) != EOK) {
                        break;
                    }
                    ctp->rcvid = rcvid2;
                    locked = 1;
                }
                n = func(ctp, msg, binding->ocb);
            } else {
                n = ENOSYS;
            }
            break;
        }

        ctp->rcvid = rcvid;
        if (combine == 0 || n != EOK) {
            break;
        }

        ctp->status = 0;
        msg = (ioserv_msg_t *) ((char *) msg + combine);
        n = EBADMSG;
    }

    if (locked) {
        _ioserv_unlock(ctp, binding);
    }
    _ioserv_close_handler(ctp, binding);
    return n;
}
