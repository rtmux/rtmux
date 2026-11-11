/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <minmax.h>
#include <sys/rtmux-core.h>
#include <stddef.h>
#include <stdlib.h>
#include <sys/iomsg.h>
#include <sys/ioserv-private.h>
#include <sys/path-registry.h>
#include <sys/dispatch.h>
#include <string.h>
#include <pthread.h>
#include "state.h"
#include "../ioserv/api.h"
#include "../ioserv/internal.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif
#define MSG_MAX_SIZE                         \
    (sizeof(struct _io_connect_link_reply) + \
     sizeof(struct _io_connect_entry) * SYMLOOP_MAX + PATH_MAX + 1)

int ioserv_backend_attach(dispatch_t *dpp,
                          ioserv_attr_t *attr,
                          const char *path,
                          enum _file_type file_type,
                          unsigned flags,
                          const ioserv_connect_ops_t *connect_funcs,
                          const ioserv_ops_t *io_funcs,
                          void *handle)
{
    _ioserv_control *ctrl;
    ioserv_attr_t null_attr;
    int rc;

    if (!attr) {
        memset(attr = &null_attr, 0, sizeof null_attr);
    }

    if (!_DPP(dpp)->ioserv_ctrl) {
        message_attr_t msg_attr;

        if (!(ctrl = malloc(sizeof *ctrl))) {
            errno = ENOMEM;
            return -1;
        }
        memset(ctrl, 0, sizeof *ctrl);

        if ((rc = pthread_mutex_init(&ctrl->mutex, 0)) != EOK) {
            free(ctrl);
            ;
            errno = rc;
            return -1;
        }

        memset(&msg_attr, 0, sizeof msg_attr);
        msg_attr.flags = MSG_FLAG_TYPE_IOSERV;

        ctrl->nparts_max = max_uint(attr->nparts_max, 1u);
        ctrl->msg_max_size = (unsigned) max_size(
            attr->msg_max_size, max_size(MSG_MAX_SIZE, sizeof(ioserv_msg_t)));

        ctrl->context_size =
            (unsigned) (sizeof(ioserv_context_t) +
                        ctrl->nparts_max *
                            sizeof(((ioserv_context_t *) 0)->iov[0]) +
                        ctrl->msg_max_size);

        if (_dispatch_attach(dpp, ctrl, DISPATCH_IOSERV) == -1) {
            pthread_mutex_destroy(&ctrl->mutex);
            free(ctrl);
            return -1;
        }
        /* Attach message type as well */
        if (message_attach(dpp, &msg_attr, _IO_BASE, _IO_MAX,
                           _ioserv_msg_handler, NULL) == -1) {
            return -1;
        }

        msg_attr.flags |= MSG_FLAG_TYPE_NOTIFICATION;

        /* Attach I/O service notification types. */
        if (message_attach(dpp, &msg_attr, _NOTIFICATION_CODE_DISCONNECT,
                           _NOTIFICATION_CODE_DISCONNECT, _ioserv_msg_handler,
                           NULL) == -1) {
            return -1;
        }

        if (message_attach(dpp, &msg_attr, _NOTIFICATION_CODE_UNBLOCK,
                           _NOTIFICATION_CODE_UNBLOCK, _ioserv_msg_handler,
                           NULL) == -1) {
            return -1;
        }

    } else {
        unsigned newsize, newcsize, newiov;

        ctrl = dpp->ioserv_ctrl;

        newiov = max_uint(attr->nparts_max, ctrl->nparts_max);
        newsize = (unsigned) max_size(
            ctrl->msg_max_size,
            max_size(attr->msg_max_size,
                     max_size(MSG_MAX_SIZE, sizeof(ioserv_msg_t))));

        newcsize =
            (unsigned) (sizeof(ioserv_context_t) +
                        newiov * sizeof(((ioserv_context_t *) 0)->iov[0]) +
                        newsize);

        if ((dpp->flags & _DISPATCH_CONTEXT_ALLOCED) &&
            (newcsize > ctrl->context_size || newsize > ctrl->msg_max_size)) {
            errno = EINVAL;
            return -1;
        } else {
            ctrl->nparts_max = newiov;
            ctrl->context_size = newcsize;
            ctrl->msg_max_size = newsize;
        }

        if (_dispatch_set_contextsize(dpp, DISPATCH_IOSERV) == -1) {
            errno = EINVAL;
            return -1;
        }
    }

    /* Allow service attach with a NULL path, just to set up the internals. */
    if (path || (flags & IOSERV_FLAG_FTYPEONLY)) {
        struct link *linkl;

        if (!(linkl = _ioserv_link_alloc())) {
            errno = ENOMEM;
            return -1;
        }

        if (attr->other_func && !ctrl->other_func) {
            linkl->flags |= _IOSERV_LINK_OTHERFUNC;
            ctrl->other_func = attr->other_func;
            _DPP(dpp)->other_func = (void *) attr->other_func;
        }

        linkl->connect_funcs = connect_funcs;
        linkl->io_funcs = io_funcs;
        linkl->handle = handle;
        if ((linkl->link_id = namespace_link(
                 path, 0, 0, _DPP(dpp)->chid, (unsigned) linkl->id, file_type,
                 flags & (unsigned) IOSERV_FLAG_MASK)) == -1) {
            if (linkl->flags & _IOSERV_LINK_OTHERFUNC) {
                dpp->other_func = NULL;
            }
            _ioserv_link_free(linkl->id, IOSERV_DETACH_ALL);
            return -1;
        }
        linkl->flags &= ~(unsigned) _IOSERV_LINK_HALFOPEN;
        return linkl->id;
    }
    return 0;
}

ioserv_context_t *ioserv_backend_context_alloc(dispatch_t *dpp)
{
    return (ioserv_context_t *) dispatch_context_alloc(dpp);
}

void ioserv_backend_context_free(ioserv_context_t *ctp)
{
    free(ctp);
}

ioserv_context_t *ioserv_backend_block(ioserv_context_t *ctp)
{
    /* @@ Check if context size has changed, is so, realloc */
    ctp->id = -1;
    ctp->info.msglen = 0;

again:
    ctp->rcvid =
        msg_receive(ctp->dpp->chid, ctp->msg, ctp->msg_max_size, &ctp->info);

    /* Doing a network transaction and not all the message was send, so get the
     * rest...
     */
    if (ctp->rcvid > 0 && ctp->info.srcmsglen > ctp->info.msglen &&
        ctp->info.msglen < ctp->msg_max_size) {
        int n;

        if ((n = msg_read_r(ctp->rcvid, (char *) ctp->msg + ctp->info.msglen,
                            (int) (ctp->msg_max_size - ctp->info.msglen),
                            (int) ctp->info.msglen)) < 0) {
            msg_error(ctp->rcvid, -n);
            goto again;
        }
        ctp->info.msglen += (unsigned) n;
    }

    return ctp;
}

int ioserv_backend_handler(ioserv_context_t *ctp)
{
    _ioserv_handler(ctp);
    return 0;
}

void ioserv_backend_unblock(ioserv_context_t *ctp)
{
    _message_unblock((dispatch_context_t *) ctp);
}

/* This gets called from the message interface if we have both I/O service and
 * message types registered.
 */
int _ioserv_msg_handler(message_context_t *ctp,
                        int code,
                        unsigned flags,
                        void *handle)
{
    _ioserv_handler((ioserv_context_t *) ctp);
    return 0;
}
