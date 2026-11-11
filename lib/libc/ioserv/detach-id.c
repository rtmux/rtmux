/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <sys/ioserv-private.h>
#include <atomic.h>
#include "internal.h"
#include <string.h>
#ifndef NDEBUG
#include <assert.h>
#endif


static int ebadf_reply(ioserv_context_t *ctp, void *msg, IOSERV_OCB_T *ocb)
{
    return EBADF;
}

static const ioserv_ops_t ebadf_io_funcs = {
    .nfuncs = IOSERV_IO_NFUNCS,
    .read = (void *) ebadf_reply,
    .write = (void *) ebadf_reply,
    .stat = (void *) ebadf_reply,
    .notify = (void *) ebadf_reply,
    .devctl = (void *) ebadf_reply,
    .unblock = (void *) ebadf_reply,
    .pathconf = (void *) ebadf_reply,
    .lseek = (void *) ebadf_reply,
    .chmod = (void *) ebadf_reply,
    .chown = (void *) ebadf_reply,
    .utime = (void *) ebadf_reply,
    .openfd = (void *) ebadf_reply,
    .fdinfo = (void *) ebadf_reply,
    .lock = (void *) ebadf_reply,
    .space = (void *) ebadf_reply,
    .reserved_shutdown = (void *) ebadf_reply,
    .mmap = (void *) ebadf_reply,
    .msg = (void *) ebadf_reply,
    .reserved_unused = (void *) ebadf_reply,
    .dup = (void *) ebadf_reply,
    .lock_ocb = (void *) ebadf_reply,
    .unlock_ocb = (void *) ebadf_reply,
    .sync = (void *) ebadf_reply,
};

/* This is an ugly way to do the detach since it requires that we know both
 * about the locks and size of the table managed by _ioserv_handle() as well as
 * locking the entire data structure for a longer period of time. It is a
 * cheaper solution however then the second block of code, which would require
 * additional changes to the _ioserv_handle() code to not shift the found
 * bindings around to the head of the list.
 *
 * It still would be better to have this entire operation encapsulated inside of
 * _ioserv_handle() since both implementations rely on _ioserv_io_table.nentries
 * being known, at least the code below is honest about breaking the
 * encapsulation. This code is _almost_ the same as the code in ioserv_detach().
 */
int _ioserv_detach_id(ioserv_context_t *ctp, int id, unsigned flags)
{
    struct binding *binding;
    struct binding *current_binding;
    struct _ioserv_handle_list *list;
    struct _ioserv_handle_entry *entry, *nentry;
    int found = 0;
    enum _ioserv_handle_type type = _IOSERV_HANDLE_FIND_LOCK;
    _ioserv_func_t func;
    ioserv_msg_t *saved_msgs;
    struct _msg_info saved_info;
    int saved_rcvid;
    const ioserv_ops_t *saved_funcs = NULL;
    io_close_t close;
    unsigned scoid;

    /* Something on this thread's stack. Should be unique for this thread to
     * mark paths (via coids) to bindings it already has locked. Can't use
     * something like NULL in case 2 threads doing ioserv_backend_detach() at
     * same time.
     */
    int marker;


    saved_msgs = ctp->msg;
    saved_rcvid = ctp->rcvid;
    memcpy(&saved_info, &ctp->info, sizeof ctp->info);

    /* Set the following 2 for close callout */
    ctp->rcvid = -1; /* In case they try to reply to someone */
    ctp->msg_max_size = (unsigned) sizeof close;
    ctp->info.msglen = (int32_t) sizeof close;

    close.i.type = _IO_CLOSE;
    close.i.combine_len = sizeof close.i;

    ctp->msg = (ioserv_msg_t *) &close;

    current_binding = NULL;
    found = 0;

    __mutex_lock(&_ioserv_io_table.mutex);
loop_again:

    for (list = _ioserv_io_table.vector, scoid = 0;
         scoid < _ioserv_io_table.nentries; list++, scoid++) {
        ctp->info.scoid = (int32_t) (scoid | RTMUX_SIDE_CHANNEL);
        for (entry = list->list; entry; entry = nentry) {
            nentry = entry->next;


            binding = (struct binding *) entry->handle;

            if (type == _IOSERV_HANDLE_FIND_LOCK) {
                if (entry->handle == &marker ||
                    binding->funcs == &ebadf_io_funcs) {
                    /* Either we've done the locking phase on this binding
                     * (first test), or we've done both lock, unlock phases
                     * (second test).
                     */
                    continue;
                }
            } else {
                if (entry->handle != &marker) {
                    /* We've already unlocked this one (or never had it locked)
                     */
                    continue;
                }
                /* restore */
                entry->handle = current_binding;
                binding = current_binding;
            }


            if ((!current_binding && binding->id == id) ||
                binding == current_binding) {
                ctp->info.coid = entry->coid & ~_IOSERV_HANDLE_LOCK;
                ctp->info.pid = list->pid;
                ctp->info.nd = list->nd;
                ctp->info.tid = 0;
                __mutex_unlock(&_ioserv_io_table.mutex);

                binding = _ioserv_handle(&ctp->info, NULL, type);

                if (type == _IOSERV_HANDLE_FIND_LOCK) {
                    if (binding != (void *) -1) {
                        /* Good idea to recheck in case something changed in the
                         * interim
                         */
                        if (binding->id != id ||
                            binding->funcs == &ebadf_io_funcs ||
                            (current_binding && binding != current_binding)) {
                            _ioserv_handle(&ctp->info, NULL,
                                           _IOSERV_HANDLE_UNLOCK);
                        } else {
                            current_binding = binding;
                            found++;
                            entry->handle = &marker;
                        }
                    }
                } else {
                    found--;
                    if (flags & IOSERV_DETACH_CLOSE) {
                        /* Do close callout(s) */

                        if ((func = _ioserv_io_func(saved_funcs, _IO_CLOSE))) {
                            (void) func(ctp, ctp->msg, binding->ocb);
                        }

                        /* The CLOSE_OCB callout is bassed on found, not
                         * binding->count because we always want it to be called
                         * if present. The last decrement of binding->count may
                         * happen on real _IO_CLOSE message handling. By that
                         * time, function table is already overwritten.
                         */
                        if (found == 0 &&
                            (func = _ioserv_io_func(saved_funcs,
                                                    _IO_RSVD_CLOSE_OCB))) {
                            (void) func(ctp, 0, binding->ocb);
                        }
                    }

                    /* Remove the binding, not just decrement the ref, while a
                     * connection request could still hold a stale ocb handle.
                     */
                    if (_ioserv_handle(&ctp->info, binding,
                                       _IOSERV_HANDLE_REMOVE_LOCK) !=
                            (void *) -1 &&
                        (atomic_sub_value(&binding->count, 1) == 1)) {
                        free(binding);
                    }
                }
                __mutex_lock(&_ioserv_io_table.mutex);
            }
        }
    }

    if (type == _IOSERV_HANDLE_UNLOCK) {
        current_binding = NULL;
#ifndef NDEBUG
        assert(found == 0);
#endif
        found = 0;
        type = _IOSERV_HANDLE_FIND_LOCK;
        goto loop_again;
    } else if (found) {
        /* Atomic in case multiple ioserv_backend_detach() happening on same id
         */
        atomic_add(&current_binding->count, (unsigned) found);

        /* We're about to do the unlocking so overwrite the binding's funcs.
         * Everyone now gets EBADF on whatever (even IO_CLOSE for which the
         * callouts will be done just after the unlock).
         */
        saved_funcs = current_binding->funcs;
        current_binding->funcs = &ebadf_io_funcs;
        type = _IOSERV_HANDLE_UNLOCK;
        goto loop_again;
    }

    __mutex_unlock(&_ioserv_io_table.mutex);

    ctp->msg = saved_msgs;
    ctp->rcvid = saved_rcvid;
    memcpy(&ctp->info, &saved_info, sizeof ctp->info);

    return 0;
}
