/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/dispatch.h>
#include <sys/ioserv-private.h>
#include <errno.h>
#include <minmax.h>
#include <pthread.h>
#include <stddef.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include "state.h"

#define GROW_VEC 4
#define MSG_MAX_SIZE sizeof(struct _notification)

static message_vec_t *message_vector_find_free(message_vec_t *vec, unsigned num)
{
    for (unsigned i = 0; i < num; i++) {
        if ((vec[i].flags & _VEC_VALID) == 0)
            return &vec[i];
    }
    return NULL;
}

int message_attach(
    dispatch_t *dpp,
    message_attr_t *attr,
    int low,
    int high,
    int (*func)(message_context_t *ctp, int fd, unsigned flags, void *handle),
    void *handle)
{
    message_vec_t *vec, *new_vec;
    unsigned num_elem, num_used;
    int rc = 0;
    _message_control *ctrl;

    if (!_DPP(dpp)->message_ctrl) {
        if (!(ctrl = malloc(sizeof *ctrl))) {
            errno = ENOMEM;
            return -1;
        }
        memset(ctrl, 0, sizeof *ctrl);

        if ((rc = pthread_mutex_init(&ctrl->mutex, 0)) != EOK) {
            free(ctrl);
            errno = rc;
            return -1;
        }

        ctrl->nparts_max = max_uint(attr ? attr->nparts_max : 0, 1u);
        ctrl->msg_max_size =
            (unsigned) max_size(attr ? attr->msg_max_size : 0,
                                max_size(MSG_MAX_SIZE, sizeof(ioserv_msg_t)));

        ctrl->context_size =
            (unsigned) (sizeof(message_context_t) +
                        ctrl->nparts_max *
                            sizeof(((ioserv_context_t *) 0)->iov[0]) +
                        ctrl->msg_max_size);

        if (_dispatch_attach(dpp, ctrl, DISPATCH_MESSAGE) == -1) {
            pthread_mutex_destroy(&ctrl->mutex);
            free(ctrl);
            return -1;
        }
    }

    ctrl = _DPP(dpp)->message_ctrl;
    pthread_mutex_lock(&ctrl->mutex);

    vec = new_vec = _DPP(dpp)->message_ctrl->message_vec;
    num_elem = _DPP(dpp)->message_ctrl->num_elements;
    num_used = _DPP(dpp)->message_ctrl->num_entries;

    /* Attach message type to message vector */
    if (!vec || num_elem == num_used) {
        new_vec = realloc(vec, (unsigned) (num_elem + GROW_VEC) * sizeof(*vec));
        if (!new_vec) {
            errno = ENOMEM;
            pthread_mutex_unlock(&ctrl->mutex);
            return -1;
        }
        memset(&new_vec[num_elem], 0, GROW_VEC * sizeof(*vec));
        _DPP(dpp)->message_ctrl->num_elements = num_elem = GROW_VEC + num_elem;
        _DPP(dpp)->message_ctrl->message_vec = new_vec;
    }

    vec = message_vector_find_free(new_vec, num_elem);
    if (!vec) {
        /* @@@ fprintf(stderr, "error\n"); */
        errno = ENOMEM;
        pthread_mutex_unlock(&ctrl->mutex);
        return -1;
    }

    if (attr && (attr->flags & MSG_FLAG_TYPE_NOTIFICATION) &&
        (attr->flags & MSG_FLAG_ALLOC_NOTIFICATION)) {
        /* We need to allocate a notification code */
        message_vec_t *vec2;
        int i;
        low = _NOTIFICATION_CODE_MINAVAIL;

        while (low <= _NOTIFICATION_CODE_MAXAVAIL) {
            high = low;
            vec2 = _DPP(dpp)->message_ctrl->message_vec;

            for (i = 0; i < _DPP(dpp)->message_ctrl->num_elements;
                 vec2++, i++) {
                if ((vec2->flags & _VEC_VALID) &&
                    (vec2->flags & _MESSAGE_NOTIFICATION_ENTRY) &&
                    (low >= vec2->lo && low <= vec2->high)) {
                    low++;
                    break;
                }
            }
            if (low == high)
                break;
        }

        if (low > _NOTIFICATION_CODE_MAXAVAIL) {
            /* Could not allocate a notification */
            pthread_mutex_unlock(&ctrl->mutex);
            errno = EAGAIN;
            return -1;
        }
        rc = low;

    } else if (attr && (attr->flags & MSG_FLAG_TYPE_NOTIFICATION) &&
               (low < SCHAR_MIN || high > SCHAR_MAX || low > high)) {
        /* Notification codes deliver as int8_t; any out-of-range or inverted
         * range can never match, so reject it instead of registering a dead
         * entry.
         */
        pthread_mutex_unlock(&ctrl->mutex);
        errno = EINVAL;
        return -1;
    } else if (attr && (attr->flags & MSG_FLAG_TYPE_NOTIFICATION) &&
               (low >= _NOTIFICATION_CODE_COIDDEATH) &&
               (high <= _NOTIFICATION_CODE_COIDDEATH) &&
               !(dpp->flags & _DISPATCH_CHANNEL_COIDDEATH)) {
        pthread_mutex_unlock(&ctrl->mutex);
        errno = EBUSY;
        return -1;
    }

    vec->lo = (short) low;
    vec->high = (short) high;
    vec->flags = _VEC_VALID;
    vec->handle = handle;
    vec->func = func;
    _DPP(dpp)->message_ctrl->num_entries++;

    if (attr) {
        if (attr->flags & MSG_FLAG_TYPE_IOSERV) {
            vec->flags |= _MESSAGE_IOSERV_ENTRY;
        }

        if (attr->flags & MSG_FLAG_TYPE_NOTIFICATION) {
            vec->flags |= _MESSAGE_NOTIFICATION_ENTRY;
            rc = low;
        }
    }

    pthread_mutex_unlock(&ctrl->mutex);

    return rc;
}

int notification_attach(
    dispatch_t *dpp,
    int flags,
    int code,
    int (*func)(message_context_t *ctp, int fd, unsigned flags, void *handle),
    void *handle)
{
    message_attr_t attr = {
        .flags = 0,
        .nparts_max = 0,
        .msg_max_size = 0,
    };

    if (flags & MSG_FLAG_ALLOC_NOTIFICATION) {
        attr.flags = (MSG_FLAG_TYPE_NOTIFICATION | MSG_FLAG_ALLOC_NOTIFICATION);
    } else {
        attr.flags = MSG_FLAG_TYPE_NOTIFICATION;
    }

    return message_attach(dpp, &attr, code, code, func, handle);
}

int message_connect(dispatch_t *dpp, int flags)
{
    return connect_attach(
        0, 0, _DPP(dpp)->chid,
        (flags & MSG_FLAG_SIDE_CHANNEL) ? RTMUX_SIDE_CHANNEL : 0, 0);
}


void _message_unblock(dispatch_context_t *ctp)
{
    dispatch_t *dpp = ((message_context_t *) ctp)->dpp;
    struct sched_param param;
    int coid;


    if ((coid = connect_attach(0, 0, dpp->chid, RTMUX_SIDE_CHANNEL, 0)) == -1) {
        return; /* We should indicate an error, but we can't */
    }

    memset(&param, 0, sizeof(param));
    if (__sched_get(0, 0, &param) == -1) {
        param.sched_priority = 1;
    }

    /* An unblock notification with rcvid == 0 should be treated as a noop
     * msg_send_notification(coid, param.sched_priority,
     * _NOTIFICATION_CODE_UNBLOCK, getpid());
     */
    (void) msg_send_notification(coid, param.sched_priority,
                                 _NOTIFICATION_CODE_UNBLOCK, 0);

    __connect_detach((uint64_t) coid);
}

/* Convenience macros. We can avoid locking the list of registered message types
 * if the NOLOCK flag was specified to dispatch_create_channel(). This is used
 * by Proc, where we know the message vector will never change.
 */

#define LOCK(dpp, mutex)                            \
    if (!(_DPP(dpp)->flags & DISPATCH_FLAG_NOLOCK)) \
    pthread_mutex_lock(mutex)
#define UNLOCK(dpp, mutex)                          \
    if (!(_DPP(dpp)->flags & DISPATCH_FLAG_NOLOCK)) \
    pthread_mutex_unlock(mutex)

int _message_handler(dispatch_context_t *dctp)
{
    message_context_t *ctp = &dctp->message_context;
    int i;
    message_vec_t *vec;
    unsigned short code;
    pthread_mutex_t *mutex = &_DPP(ctp->dpp)->message_ctrl->mutex;

    if (ctp->rcvid == -1)
        return -1;

    if (ctp->rcvid && ctp->info.msglen < 2) {
        msg_error(ctp->rcvid, EINVAL);
        return -1;
    }
    code = ctp->msg->type;

    if ((ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) && ctp->rcvid) {
        msg_error(ctp->rcvid, ENOTSUP);
        return -1;
    }

    /* Loop through the vectors, and call a registered function */
    LOCK(_DPP(ctp->dpp), mutex);

    vec = _DPP(ctp->dpp)->message_ctrl->message_vec;

    for (i = 0; i < _DPP(ctp->dpp)->message_ctrl->num_elements; vec++, i++) {
        if (vec->flags & _VEC_VALID) {
            /* Check if we have a match Notification codes are treated as int8
             * entities, while the message codes are treated as uint16 entities.
             * As a result we leave the range defined as a int16 (in state.h)
             * but cast it to a uint16 when we check the message range.
             */
            if (ctp->rcvid == 0 && (vec->flags & _MESSAGE_NOTIFICATION_ENTRY) &&
                code == _NOTIFICATION_TYPE &&
                ctp->msg->notification.subtype == _NOTIFICATION_SUBTYPE) {
                if (ctp->msg->notification.code >= vec->lo &&
                    ctp->msg->notification.code <= vec->high) {
                    UNLOCK(_DPP(ctp->dpp), mutex);
                    return vec->func(ctp, ctp->msg->notification.code, 0,
                                     vec->handle);
                }
            } else if (ctp->rcvid &&
                       !(vec->flags & _MESSAGE_NOTIFICATION_ENTRY) &&
                       code >= (uint16_t) vec->lo &&
                       code <= (uint16_t) vec->high) {
                UNLOCK(_DPP(ctp->dpp), mutex);
                return vec->func(ctp, code, 0, vec->handle);
            }
        }
    }

    UNLOCK(_DPP(ctp->dpp), mutex);

    if (ctp->rcvid) {
        msg_error(ctp->rcvid, ENOSYS);
    }

    return -1;
}
