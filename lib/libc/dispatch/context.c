/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <minmax.h>
#include <pthread.h>
#include <sys/rtmux-core.h>
#include <unistd.h>
#include <sys/dispatch.h>
#include <string.h>
#include "state.h"

static int _dispatch_coiddeath_attached = 0;

dispatch_t *dispatch_create_channel(int chid, unsigned flags)
{
    dispatch_t *dpp;

    if (!(dpp = malloc(sizeof *dpp))) {
        return NULL;
    }
    memset(dpp, 0x00, sizeof *dpp);

    dpp->chid = chid;
    dpp->flags = flags;

    return dpp;
}

dispatch_t *dispatch_create(void)
{
    return dispatch_create_channel(-1, 0);
}

dispatch_context_t *dispatch_context_alloc(dispatch_t *dpp)
{
    dispatch_context_t *ctp;

    if (_DPP(dpp)->block_type == _DISPATCH_BLOCK_RECEIVE) {
        if (!_DPP(dpp)->message_ctrl) {
            errno = EINVAL;
            return NULL;
        }

        if (!(ctp = calloc(1, _DPP(dpp)->context_size))) {
            errno = ENOMEM;
            return NULL;
        }
        ctp->message_context.dpp = (dispatch_t *) dpp;
        ctp->message_context.msg =
            (ioserv_msg_t *) ((char *) ctp + offsetof(message_context_t, iov) +
                              _DPP(dpp)->nparts_max *
                                  sizeof(ctp->message_context.iov[0]));
        ctp->message_context.msg_max_size =
            (unsigned) (_DPP(dpp)->context_size -
                        (offsetof(message_context_t, iov) +
                         _DPP(dpp)->nparts_max *
                             sizeof(ctp->message_context.iov[0])));

        dpp->flags |= _DISPATCH_CONTEXT_ALLOCED;
        return ctp;
    }

    errno = EINVAL;
    return NULL;
}

void dispatch_context_free(dispatch_context_t *ctp)
{
    free(ctp);
}

int _dispatch_attach(dispatch_t *dpp, void *ctrl, unsigned attach_type)
{
    void **ctrlptr = NULL;
    unsigned new_context_size = 0;
    unsigned new_msg_size = 0;

    /* Check validity of type and make sure it is compatible with other attached
     * types.
     */

    if (dpp->block_type) {
        switch (attach_type) {
        case DISPATCH_MESSAGE:
            dpp->flags &= ~(unsigned) _DISPATCH_ONLY_IOSERV;
            /* Fall Through */
        case DISPATCH_IOSERV:
            dpp->block_type = _DISPATCH_BLOCK_RECEIVE;
            break;
        default:
            break;
        }
    } else {
        switch (attach_type) {
        case DISPATCH_IOSERV:
            dpp->flags |= _DISPATCH_ONLY_IOSERV;
            /* Fall Through */
        case DISPATCH_MESSAGE:
            dpp->block_type = _DISPATCH_BLOCK_RECEIVE;
            break;
        default:
            break;
        }
    }

    switch (attach_type) {
    case DISPATCH_IOSERV:
        ctrlptr = (void **) &dpp->ioserv_ctrl;
        new_context_size = ((_ioserv_control *) ctrl)->context_size;
        new_msg_size = ((_ioserv_control *) ctrl)->msg_max_size;
        break;

    case DISPATCH_MESSAGE:
        ctrlptr = (void **) &dpp->message_ctrl;
        new_context_size = ((_message_control *) ctrl)->context_size;
        new_msg_size = ((_message_control *) ctrl)->msg_max_size;
        break;

    default:
        errno = EINVAL;
        return -1;
    }

    /* @@@ Could have channel flags for the chid?! */
    if (dpp->block_type == _DISPATCH_BLOCK_RECEIVE && dpp->chid == -1) {
        unsigned flags = RTMUX_FLG_CHF_UNBLOCK | RTMUX_FLG_CHF_DISCONNECT;

        if (!_dispatch_coiddeath_attached) {
            flags |= RTMUX_FLG_CHF_COID_DISCONNECT;
        }
        if ((dpp->chid = channel_create(flags)) == -1) {
            return -1;
        }
        if (!_dispatch_coiddeath_attached) {
            _dispatch_coiddeath_attached = 1;
            dpp->flags |= _DISPATCH_CHANNEL_COIDDEATH;
        }
    }

    *ctrlptr = ctrl;
    dpp->context_size = max(dpp->context_size, new_context_size);
    dpp->msg_max_size = max(dpp->msg_max_size, new_msg_size);
    dpp->nparts_max =
        max(dpp->nparts_max, ((_ioserv_control *) ctrl)->nparts_max);
    return 0;
}

static dispatch_context_t *dispatch_block_receive_all(
    dispatch_context_t *ctp,
    int (
        *block_func)(int chid, void *msg, size_t bytes, struct _msg_info *info))
{
    dispatch_t *dpp = ((message_context_t *) ctp)->dpp;

    ctp->message_context.id = -1;
    ctp->message_context.info.msglen = 0;

again:
    if ((ctp->message_context.rcvid =
             block_func(dpp->chid, ctp->message_context.msg,
                        ctp->message_context.msg_max_size,
                        &ctp->message_context.info)) == -1 &&
        errno != ETIMEDOUT) {
        return NULL;
    }

    /* Doing a network transaction and not all the message was send, so get the
     * rest...
     */
    if (ctp->message_context.rcvid > 0 &&
        ctp->message_context.info.srcmsglen >
            ctp->message_context.info.msglen &&
        ctp->message_context.info.msglen < ctp->message_context.msg_max_size) {
        int n;

        if ((n = msg_read_r(ctp->message_context.rcvid,
                            (char *) ctp->message_context.msg +
                                ctp->message_context.info.msglen,
                            (int) (ctp->message_context.msg_max_size -
                                   ctp->message_context.info.msglen),
                            (int) ctp->message_context.info.msglen)) < 0) {
            msg_error(ctp->message_context.rcvid, -n);
            goto again;
        }
        ctp->message_context.info.msglen += (uint32_t) n;
    }

    return ctp;
}

dispatch_context_t *dispatch_block(dispatch_context_t *ctp)
{
    dispatch_t *dpp = ((message_context_t *) ctp)->dpp;

    if (dpp->block_type == _DISPATCH_BLOCK_RECEIVE) {
        return dispatch_block_receive_all(ctp, msg_receive);
    }
    errno = EINVAL;
    return NULL;
}

int dispatch_handler(dispatch_context_t *ctp)
{
    dispatch_t *dpp;

    /* A missing context can occur if the blocking function came out early due
     * to a signal or other error condition and we should return -1.
     *
     * In the case of a thread pool, a -1 return from the handler function
     * indicates that a reblock should occur (what else can we do?).
     */
    if (!ctp) {
        return -1;
    }

    dpp = ((message_context_t *) ctp)->dpp;
    switch (dpp->block_type) {
    case _DISPATCH_BLOCK_RECEIVE:
        return _message_handler(ctp);
    default:
        break;
    }

    return -1;
}

int dispatch_destroy(dispatch_t *dpp)
{
    if (_DPP(dpp)->chid != -1) {
        (void) channel_destroy(_DPP(dpp)->chid);
    }

    if (dpp->flags & _DISPATCH_CHANNEL_COIDDEATH) {
        _dispatch_coiddeath_attached = 0;
    }

    /**
     * We have to go through each of the control structures here and make sure
     * that we release the resources that each one acquired. This is different
     * from what happens in a xxxx_detach() since at that point we are only
     * cleaning up a single service, not the whole of dispatch.
     */
    if (dpp->ioserv_ctrl) {
        pthread_mutex_destroy(&dpp->ioserv_ctrl->mutex);
        free(dpp->ioserv_ctrl);
    }

    if (dpp->message_ctrl) {
        /* @@@ No need for locking since someone must sync this dispatch
         * operation. pthread_mutex_lock(&dpp->message_ctrl->mutex);
         */
        if (dpp->message_ctrl->message_vec) {
            free(dpp->message_ctrl->message_vec);
        }
        /* pthread_mutex_unlock(&dpp->message_ctrl->mutex); */
        pthread_mutex_destroy(&dpp->message_ctrl->mutex);
        free(dpp->message_ctrl);
    }

    /* Just be sure we don't do this again. */
    memset(dpp, 0, sizeof(dpp));

    free(dpp);
    return 0;
}

int _dispatch_set_contextsize(dispatch_t *dpp, unsigned attach_type)
{
    unsigned new_size = 0, new_msg_size = 0;

    switch (attach_type) {
    case DISPATCH_IOSERV:
        new_size = _DPP(dpp)->ioserv_ctrl->context_size;
        new_msg_size = _DPP(dpp)->ioserv_ctrl->msg_max_size;
        break;

    case DISPATCH_MESSAGE:
        new_size = _DPP(dpp)->message_ctrl->context_size;
        new_msg_size = _DPP(dpp)->message_ctrl->msg_max_size;
        break;
    default:
        return -1;
    }

    if ((dpp->flags & _DISPATCH_CONTEXT_ALLOCED) &&
        (new_size > dpp->context_size || new_msg_size > dpp->msg_max_size)) {
        return -1;
    } else {
        /* @@@ Could have channel flags for the chid?! */
        _DPP(dpp)->context_size = max(_DPP(dpp)->context_size, new_size);
        _DPP(dpp)->msg_max_size = max(_DPP(dpp)->msg_max_size, new_msg_size);
    }

    return 0;
}
