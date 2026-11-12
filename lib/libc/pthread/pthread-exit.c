/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <limits.h>
#include <pthread.h>
#include <atomic.h>
#include <sys/rtmux-core.h>
#include "pthread-key.h"
#include "cpucfg.h"
#include "signal-state.h"
/* xmtx.h removed — Dinkumware, not needed */

void pthread_exit(void *value_ptr)
{
    struct __cleanup_handler *handler;
    int loop, again;
    volatile struct _thread_local_storage *tls = LIBC_TLS();

    /* Disable pthread_cancel and set cancel type to deferred */
    atomic_or(&tls->__flags, PTHREAD_CANCEL_DISABLE);
    atomic_clr(&tls->__flags, PTHREAD_CANCEL_ASYNCHRONOUS); /* make defered */

    /* A cancelled thread never returns through the blocking wrapper it was in;
     * drop its wait registration before anything can run on its stack.
     */
    __libc_wait_purge((int) pthread_self());

    /* call the cleanup handlers */
    while ((handler = tls->__cleanup)) {
        tls->__cleanup = handler->__next;
        handler->__routine(handler->__save);
    }

    /* Call the thread_specific_data destructor functions */
    for (loop = 0, again = 1; again && loop < PTHREAD_DESTRUCTOR_ITERATIONS;
         loop++) {
        unsigned i;
        unsigned num;

        again = 0;
        num = tls->__numkeys;
        for (i = 0; i < num; i++) {
            void *data;
            void (*destructor)(void *);

            /* Fast path: an empty slot has no destructor to run, so skip it
             * without touching the global lock. The authoritative read below
             * re-checks under the lock, so a value set concurrently isn't lost
             * to this race (and a thread setting its own key while exiting is
             * already undefined).
             */
            if (!tls->__keydata[i])
                continue;

            /* Pair the destructor with this slot's value atomically under
             * _key_mutex — the same lock pthread_setspecific and _key_delete
             * take when writing __keydata[i]. Without it, pthread_key_delete +
             * pthread_key_create could retire and reuse slot i between the two
             * reads, making us run a recreated destructor on stale data (type
             * confusion). _key_count is published under the same lock. The lock
             * never spans the destructor call: that's user code which may
             * re-enter the key API.
             */
            pthread_mutex_lock(&_key_mutex);
            destructor =
                (i < (unsigned) _key_count) ? _key_destructor[i] : _KEY_NONE;
            data = tls->__keydata[i];
            if (data && destructor != _KEY_NONE && destructor)
                tls->__keydata[i] =
                    0; /* clear before running, under the lock */
            pthread_mutex_unlock(&_key_mutex);

            if (data && destructor != _KEY_NONE && destructor) {
                destructor(data);
                again = 1;
            }
        }
    }

    (void) thread_destroy_r(0, -1, value_ptr);
#ifdef __GNUC__
    for (;;)
        ; /* Get gcc to shut up about a noreturn function returning */
#endif
}
