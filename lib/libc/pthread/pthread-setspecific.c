/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <limits.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h> /* for memset */
#include <pthread.h>
#include "pthread-key.h"
#include "cpucfg.h"

static struct keyinfo {
    unsigned numkeys;
    void **keydata;
} *keyarray;
static unsigned keysize;
static pthread_mutex_t keymutex = PTHREAD_MUTEX_INITIALIZER;

int pthread_setspecific(pthread_key_t key, const void *value)
{
    void **temp;
    int status;
    volatile struct _thread_local_storage *tls = LIBC_TLS();
    unsigned tid_index = (unsigned) tls->__tid - 1;

    /* Hold _key_mutex across the whole update. It guards key validity against a
     * concurrent pthread_key_delete; releasing it before the value is stored
     * would let a delete+recreate slip in between validation and the write,
     * leaving a stale pointer in this slot that the wrong destructor later runs
     * on. _key_destructor is also allocated/published under _key_mutex, so the
     * read must hold it. Lock order is _key_mutex then keymutex, matching
     * pthread_key_delete (-> _key_delete), so no inversion.
     */
    if ((status = pthread_mutex_lock(&_key_mutex)) != EOK) {
        return status;
    }
    if (key < 0 || (unsigned) key >= PTHREAD_KEYS_MAX || !_key_destructor ||
        _key_destructor[key] == _KEY_NONE) {
        pthread_mutex_unlock(&_key_mutex);
        return EINVAL;
    }

    /* No other thread can access this threads data so just stuff it if it was
     * used before
     */
    if ((unsigned) key < tls->__numkeys) {
        tls->__keydata[key] = (void *) value;
        pthread_mutex_unlock(&_key_mutex);
        return EOK;
    }

    /* We will be accessing a global array, so protect it!! */
    if ((status = pthread_mutex_lock(&keymutex)) != EOK) {
        pthread_mutex_unlock(&_key_mutex);
        return status;
    }

    if (!tls->__keydata) {
        /* this is this the first pthread_setspecific for this thread */
        if (tid_index >= keysize) {
            /* this is the largest tid in the process to use pthread_setspecific
             */
            struct keyinfo *tmp;

            /* grow the array to remember the memory globaly */
            if (!(tmp =
                      realloc(keyarray, (unsigned) tls->__tid * sizeof *tmp))) {
                pthread_mutex_unlock(&keymutex);
                pthread_mutex_unlock(&_key_mutex);
                return ENOMEM;
            }

            /* initialize the new entries */
            keyarray = tmp;
            memset(&(keyarray[keysize]), 0,
                   sizeof *tmp * ((unsigned) tls->__tid - keysize));
            keysize = (unsigned) tls->__tid;
        }

        /* The tid was used before, re-use the thread data, but zero it!! */
        tls->__keydata = keyarray[tid_index].keydata;
        tls->__numkeys = keyarray[tid_index].numkeys;
        memset(tls->__keydata, 0x00, sizeof *tls->__keydata * tls->__numkeys);

        /* See if there is a spot now? */
        if ((unsigned) key < tls->__numkeys) {
            tls->__keydata[key] = (void *) value;
            pthread_mutex_unlock(&keymutex);
            pthread_mutex_unlock(&_key_mutex);
            return EOK;
        }
    }

    /* Must grow the thread specific key array */
    if (!(temp =
              realloc(tls->__keydata, ((unsigned) key + 1) * sizeof *temp))) {
        pthread_mutex_unlock(&keymutex);
        pthread_mutex_unlock(&_key_mutex);
        return ENOMEM;
    }

    /* Remember the information in both the global array and the local tls */
    keyarray[tid_index].keydata = tls->__keydata = temp;
    memset(&(tls->__keydata[tls->__numkeys]), 0,
           sizeof *temp * (((unsigned) key + 1) - tls->__numkeys));
    keyarray[tid_index].numkeys = tls->__numkeys = (unsigned) key + 1;

    /* We are done */
    tls->__keydata[key] = (void *) value;
    pthread_mutex_unlock(&keymutex);
    pthread_mutex_unlock(&_key_mutex);
    return EOK;
}

void _key_delete(pthread_key_t key)
{
    unsigned i;

    /* Reset key value if key had been previously used */
    pthread_mutex_lock(&keymutex);
    for (i = 0; i < keysize; i++) {
        if (keyarray[i].keydata && (unsigned) key < keyarray[i].numkeys) {
            keyarray[i].keydata[key] = NULL;
        }
    }
    pthread_mutex_unlock(&keymutex);
}
