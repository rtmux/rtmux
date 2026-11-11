/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <minmax.h>
#include <pthread.h>
#include <errno.h>
#include <string.h>
#include <sys/ioserv-private.h>
#include <sys/node.h>
#include <sys/rtmux-core.h>
#include "internal.h"

#define RECYCLE_ENTRY(p)                                             \
    do {                                                             \
        if (_ioserv_io_table.total > _ioserv_io_table.min &&         \
            _ioserv_io_table.free >= (unsigned) _IOSERV_OBJ_LOWAT) { \
            _ioserv_io_table.total--;                                \
            free(p);                                                 \
        } else {                                                     \
            (p)->next = _ioserv_io_table.free_list;                  \
            _ioserv_io_table.free_list = (p);                        \
            _ioserv_io_table.free++;                                 \
        }                                                            \
    } while (0)

#define RECYCLE_BUCKET(list)                                                  \
    do {                                                                      \
        if ((list)->nlists > _IOSERV_CLIENT_FD_MIN ||                         \
            (_ioserv_io_table.total_buckets > _ioserv_io_table.min_buckets && \
             _ioserv_io_table.nfree_buckets >=                                \
                 (unsigned) _IOSERV_OBJ_LOWAT)) {                             \
            _ioserv_io_table.total_buckets--;                                 \
            free((list)->lists);                                              \
        } else {                                                              \
            ((union _ioserv_entry_bucket *) ((list)->lists))->next =          \
                _ioserv_io_table.free_buckets;                                \
            _ioserv_io_table.free_buckets =                                   \
                (union _ioserv_entry_bucket *) ((list)->lists);               \
            _ioserv_io_table.nfree_buckets++;                                 \
        }                                                                     \
        (list)->lists = NULL;                                                 \
        (list)->nlists = 0;                                                   \
        (list)->nlists_max = (int) _ioserv_io_table.nlists_max;               \
    } while (0)

void *_ioserv_handle(struct _msg_info *rep,
                     void *handle,
                     enum _ioserv_handle_type type)
{
    struct _ioserv_handle_entry *p;
    struct _ioserv_handle_list *list;
    int scoid, tmp;
    int lidx;
    int lock;
    struct _ioserv_handle_entry **newlists;

    lock = type & _IOSERV_HANDLE_LOCK;
    type &= ~_IOSERV_HANDLE_LOCK;

    if (type == _IOSERV_HANDLE_SET && handle == (void *) -1) {
        errno = EINVAL;
        return (void *) -1;
    }

    __mutex_lock(&_ioserv_io_table.mutex);

    if (rep->scoid < 0) {
        for (list = _ioserv_io_table.vector, scoid = 0;
             scoid < (int) _ioserv_io_table.nentries; list++, scoid++) {
            if (list->list && rep->pid == list->pid &&
                RTMUX_NODE_CMP(rep->nd, list->nd) == 0) {
                break;
            }
        }
        if (scoid >= (int) _ioserv_io_table.nentries) {
            __mutex_unlock(&_ioserv_io_table.mutex);
            errno = ESRCH;
            return (void *) -1;
        }
    } else {
        scoid = rep->scoid & (int) ~RTMUX_SIDE_CHANNEL;
        if (scoid >= (int) _ioserv_io_table.nentries) {
            if (type != _IOSERV_HANDLE_SET) {
                __mutex_unlock(&_ioserv_io_table.mutex);
                errno = ESRCH;
                return (void *) -1;
            }
            if ((list = realloc(_ioserv_io_table.vector,
                                (unsigned) (scoid + 1) * sizeof *list))) {
                _ioserv_io_table.vector = list;
                memset(&list[_ioserv_io_table.nentries], 0x00,
                       ((unsigned) (scoid + 1) - _ioserv_io_table.nentries) *
                           sizeof *list);

                /* Set the current hash size here, when entry is reused through
                 * RECYCLE_BUCKET(). This keeps existing scoids stable and
                 * avoids checking the current hash size on every access.
                 */
                for (tmp = (int) _ioserv_io_table.nentries; tmp < scoid + 1;
                     tmp++) {
                    list[tmp].nlists_max = (int) _ioserv_io_table.nlists_max;
                }
                _ioserv_io_table.nentries = (unsigned) (scoid + 1);
            } else {
                __mutex_unlock(&_ioserv_io_table.mutex);
                errno = ENOMEM;
                return (void *) -1;
            }
        }
        list = &_ioserv_io_table.vector[scoid];
        if (type == _IOSERV_HANDLE_DISCONNECT) {
            while ((p = list->list)) {
                int status;

                do {
                    if (!(p->coid & lock)) {
                        rep->coid = p->coid;
                        rep->pid = list->pid;
                        rep->nd = list->nd;
                        rep->tid = 0;
                        p->coid |= lock;
                        handle = p->handle;
                        __mutex_unlock(&_ioserv_io_table.mutex);
                        return handle;
                    }
                } while ((p = p->next));

                list->waiting++;
                status = pthread_cond_wait(&_ioserv_io_table.cond,
                                           &_ioserv_io_table.mutex);
                /* Reset in case realloced while mutex unlocked */
                list = &_ioserv_io_table.vector[scoid];
                if (status != EOK && status != EINTR) {
                    list->waiting--;
                    __mutex_unlock(&_ioserv_io_table.mutex);
                    errno = status;
                    return (void *) -1;
                }
                list->waiting--;
            }
        } else if ((p = list->list)) {
            if (rep->pid != list->pid ||
                RTMUX_NODE_CMP(rep->nd, list->nd) != 0) {
                if (type != _IOSERV_HANDLE_SET) {
                    __mutex_unlock(&_ioserv_io_table.mutex);
                    errno = ESRCH;
                    return (void *) -1;
                }
                while ((p = list->list)) {
                    list->list = p->next;
                    RECYCLE_ENTRY(p);
                }
                RECYCLE_BUCKET(list);
                if (list->waiting) {
                    pthread_cond_broadcast(&_ioserv_io_table.cond);
                }
            }
        }
    }

    for (;;) {
        int status;

        lidx = rep->coid % list->nlists_max;

        if (lidx >= list->nlists) {
            p = NULL;
            break;
        }

        for (p = list->lists[lidx]; p; p = p->next) {
            if ((tmp = p->coid & ~_IOSERV_HANDLE_LOCK) == rep->coid) {
                break;
            }
            if (tmp % list->nlists_max != lidx) {
                p = NULL;
                break;
            }
        }
        if (!p || !(p->coid & lock)) {
            break;
        }
        list->waiting++;
        status =
            pthread_cond_wait(&_ioserv_io_table.cond, &_ioserv_io_table.mutex);
        /* Reset in case realloced while mutex unlocked */
        list = &_ioserv_io_table.vector[scoid];
        if (status != EOK && status != EINTR) {
            list->waiting--;
            __mutex_unlock(&_ioserv_io_table.mutex);
            errno = status;
            return (void *) -1;
        }
        list->waiting--;
    }

    if (p) {
        if (rep->pid != list->pid || RTMUX_NODE_CMP(rep->nd, list->nd) != 0) {
            __mutex_unlock(&_ioserv_io_table.mutex);
            errno = EINVAL;
            return (void *) -1;
        }
        if (type == _IOSERV_HANDLE_SET) {
            __mutex_unlock(&_ioserv_io_table.mutex);
            errno = EBUSY;
            return (void *) -1;
        }
    } else {
        if (type != _IOSERV_HANDLE_SET) {
            __mutex_unlock(&_ioserv_io_table.mutex);
            errno = ESRCH;
            return (void *) -1;
        }
        if (!list->list) {
            list->nd = rep->nd;
            list->pid = rep->pid;
        }

        if (lidx >= list->nlists) {
            tmp = _IOSERV_CLIENT_FD_MIN;
            while (lidx >= tmp)
                tmp <<= 1;
            tmp = min(tmp, list->nlists_max);
            if (list->nlists == 0 && tmp == _IOSERV_CLIENT_FD_MIN &&
                _ioserv_io_table.free_buckets) {
                newlists = &_ioserv_io_table.free_buckets->lists;
                _ioserv_io_table.free_buckets =
                    _ioserv_io_table.free_buckets->next;
                _ioserv_io_table.nfree_buckets--;
            } else {
                if ((newlists = realloc(list->lists,
                                        (unsigned) tmp * sizeof(*newlists))) ==
                    NULL) {
                    __mutex_unlock(&_ioserv_io_table.mutex);
                    errno = ENOMEM;
                    return (void *) -1;
                }
                _ioserv_io_table.total_buckets += (list->nlists == 0 ? 1u : 0u);
            }
            memset(&newlists[list->nlists], 0x00,
                   (unsigned) (tmp - list->nlists) * sizeof(*newlists));
            list->lists = newlists;
            list->nlists = tmp;
        }

        if ((p = _ioserv_io_table.free_list)) {
            _ioserv_io_table.free_list = p->next;
            _ioserv_io_table.free--;
        } else if ((p = malloc(sizeof *p))) {
            _ioserv_io_table.total++;
        } else {
            __mutex_unlock(&_ioserv_io_table.mutex);
            errno = ENOMEM;
            return (void *) -1;
        }


        if (!list->lists[lidx]) {
            if ((p->next = list->list))
                p->next->prev = &p->next;

            /* p->prev = &list->list is expected; however, list may be realloc'd
             * above so we can't save a pointer to it. A missing p->prev means
             * it is at the head. This means an extra if just below and one on
             * removal.
             */
            p->prev = NULL;
            list->list = p;
        } else {
            if ((p->prev = list->lists[lidx]->prev))
                *p->prev = p;
            else
                list->list = p;
            list->lists[lidx]->prev = &p->next;

            p->next = list->lists[lidx];
        }
        list->lists[lidx] = p;

        p->coid = rep->coid;
        p->handle = handle;
    }

    if (type == _IOSERV_HANDLE_REMOVE) {
        if (handle && p->handle != handle) {
            __mutex_unlock(&_ioserv_io_table.mutex);
            errno = ESRCH;
            return (void *) -1;
        }

        handle = p->handle;

        if (list->lists[lidx] == p && (list->lists[lidx] = p->next) &&
            ((p->next->coid & ~_IOSERV_HANDLE_LOCK) % list->nlists_max) !=
                lidx) {
            list->lists[lidx] = NULL;
        }

        if (p->next)
            p->next->prev = p->prev;
        if (p->prev)
            *p->prev = p->next;
        else if (!(list->list = p->next))
            RECYCLE_BUCKET(list);

        RECYCLE_ENTRY(p);
        if (list->waiting) {
            pthread_cond_broadcast(&_ioserv_io_table.cond);
        }
        __mutex_unlock(&_ioserv_io_table.mutex);
        return handle;
    }

    p->coid |= lock;

    if (type == _IOSERV_HANDLE_UNLOCK) {
        if ((p->coid & _IOSERV_HANDLE_LOCK) && list->waiting) {
            pthread_cond_broadcast(&_ioserv_io_table.cond);
        }
        p->coid &= ~_IOSERV_HANDLE_LOCK;
    }

    handle = p->handle;
    __mutex_unlock(&_ioserv_io_table.mutex);
    return handle;
}
