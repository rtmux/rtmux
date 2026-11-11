/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stddef.h>
#include <unistd.h>
#include <errno.h>
#include <atomic.h>
#include <endian.h>
#include <sys/ioserv-private.h>
#include <sys/poll.h>
#include "internal.h"

#define RTMUX_POLL_RESERVED 0x8000

static void ENDIAN_SWAPNOTIFY(struct _io_notify *notify)
{
    ENDIAN_SWAP32(&notify->service_data[0]);
    ENDIAN_SWAP32(&notify->service_data[1]);
    ENDIAN_SWAP32(&notify->flags_extra_mask);
    ENDIAN_SWAP32(&notify->flags_exten);
    ENDIAN_SWAP32(&notify->nfds);
    ENDIAN_SWAP32(&notify->fd_first);
    ENDIAN_SWAP32(&notify->nfds_ready);
    ENDIAN_SWAP64(&notify->timo);
}

static void ENDIAN_SWAPFDS(struct pollfd *fds, int n)
{
    while (n > 0) {
        ENDIAN_SWAP32(&fds->fd);
        ENDIAN_SWAP16(&fds->events);
        ENDIAN_SWAP16(&fds->revents);
        --n;
        ++fds;
    }
}

int _ioserv_notify_handler(ioserv_context_t *ctp, ioserv_msg_t *msg_in)
{
    struct binding *binding;
    int cando_upto, cando_each, todo, trailers;
    int offset_fd, offset_cur, offset_last, found, ret, first, no_gap;
    struct pollfd *fds;
    io_notify_t *msg;


    uint16_t combine_len;
    int32_t action;

    offset_last = 0;

    msg = &msg_in->notify;
    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
        ENDIAN_SWAPNOTIFY(&msg->i);

    combine_len = msg->i.combine_len;
    action = msg->i.action;

    /* First callout into lower layer */
    msg->i.flags_exten |= _NOTIFY_FLAGS_EXTEN_FIRST;

    offset_fd = msg->i.fd_first;
    cando_each =
        (int) ((ctp->msg_max_size - ((unsigned) ctp->offset + sizeof(msg->i))) /
               sizeof(struct pollfd));
    todo = msg->i.nfds - offset_fd;

    cando_upto = cando_each;
    offset_cur = offset_fd;
    fds = (struct pollfd *) (&msg->i + 1);
    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
        ENDIAN_SWAPFDS(fds, cando_each);

    found = 0;
    first = 1;
    no_gap = 1;
    trailers = 0;

    do {
        if (offset_fd >= cando_upto) {
            if (found) {
                if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
                    ENDIAN_SWAPFDS(fds, cando_each);
                (void) msg_write(
                    ctp->rcvid, fds,
                    (int) ((unsigned) cando_each * sizeof(struct pollfd)),
                    (int) (sizeof(msg->o) +
                           (unsigned) offset_last * sizeof(struct pollfd)));
            }

            if ((ret = msg_read_r(
                     ctp->rcvid, fds,
                     (int) ((unsigned) cando_each * sizeof(struct pollfd)),
                     (int) (sizeof(msg->i) + (unsigned) offset_fd *
                                                 sizeof(struct pollfd)))) < 0) {
                return -ret;
            }
            if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
                ENDIAN_SWAPFDS(fds, cando_each);

            offset_last = offset_fd;
            found = 0;

            cando_upto += ret / (int) sizeof(struct pollfd);
            offset_cur = 0;
        }


        for (; todo > 0 && offset_fd < cando_upto;
             offset_cur++, offset_fd++, todo--) {
            if ((fds[offset_cur].events & RTMUX_POLL_RESERVED) &&
                fds[offset_cur].fd >= 0) {
                ctp->info.coid = fds[offset_cur].fd;

                if ((binding = (struct binding *) _ioserv_handle(
                         &ctp->info, 0, _IOSERV_HANDLE_FIND_LOCK)) ==
                    (void *) -1) {
                    if (first) {
                        /* The first one should be the coid the message came in
                         * on. Always knock this one down so the client doesn't
                         * retry in a loop.
                         */
                        fds[offset_cur].events &= (short) ~RTMUX_POLL_RESERVED;
                        fds[offset_cur].revents |= POLLNVAL;
                        msg->o.fd_first++;
                        found = 1;
                        trailers++;
                    } else {
                        no_gap = 0;
                        trailers = 0;
                    }
                } else {
                    fds[offset_cur].events &= (short) ~RTMUX_POLL_RESERVED;

                    {
                        unsigned ev = (unsigned short) fds[offset_cur].events;
                        msg->i.flags = ev | (ev << 28);
                    }
                    msg->i.flags = msg->i.flags | _NOTIFY_COND_EXTEN |
                                   msg->i.flags_extra_mask;

                    msg->i.combine_len = combine_len;
                    msg->i.action = action;

                    if ((ret = _ioserv_io_handler(ctp, msg_in, binding)) > 0)
                        return ret;

                    msg->i.flags_exten &= ~(unsigned) _NOTIFY_FLAGS_EXTEN_FIRST;
                    if (msg->o.flags) {
                        msg->o.flags |=
                            (msg->o.flags & ~(uint32_t) _NOTIFY_COND_EXTEN) >>
                            28;
                        fds[offset_cur].revents = (short) msg->o.flags;
                        msg->o.nfds_ready++;
                        action = _NOTIFY_ACTION_POLL; /* Don't arm any more */
                    }
                    msg->o.fd_first += no_gap;
                    found = 1;
                    trailers++;
                }
                first = 0;
            } else {
                fds[offset_cur].events &= (short) ~RTMUX_POLL_RESERVED;
                msg->o.fd_first += no_gap;
                trailers++;
            }

            msg->i.event.sigev_value.sival_int =
                (int) ((unsigned) msg->i.event.sigev_value.sival_int + 1);
            msg->i.event.sigev_value.sival_int &= (int) _NOTIFY_DATA_MASK;
        }
    } while (todo > 0);

    msg->o.nfds -= trailers;

    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
        ENDIAN_SWAPFDS(fds, cando_each);
    if (offset_fd >= cando_each) {
        (void) msg_write(ctp->rcvid, fds,
                         (int) ((unsigned) offset_cur * sizeof(struct pollfd)),
                         (int) (sizeof(msg->o) + (unsigned) offset_last *
                                                     sizeof(struct pollfd)));
        ret = (int) sizeof(msg->o);
    } else {
        ret = (int) (sizeof(msg->o) +
                     (unsigned) offset_cur * sizeof(struct pollfd));
    }

    ctp->status = 0;
    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
        ENDIAN_SWAPNOTIFY(&msg->i);
    return IOSERV_PTR(ctp, &msg->o, ret);
}
