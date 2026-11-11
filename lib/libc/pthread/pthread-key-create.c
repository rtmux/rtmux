/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <pthread.h>
#include "pthread-key.h"

int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{
    int ret;
    pthread_key_t k;

    if ((ret = pthread_mutex_lock(&_key_mutex)) != EOK) {
        return ret;
    }
    /* allocate destructor table */
    if (!_key_destructor) {
        if (!(_key_destructor =
                  malloc(sizeof *_key_destructor * PTHREAD_KEYS_MAX))) {
            pthread_mutex_unlock(&_key_mutex);
            return ENOMEM;
        }
        for (int _i = 0; _i < PTHREAD_KEYS_MAX; _i++)
            _key_destructor[_i] = _KEY_NONE;
    }
    /* is destructor valid? */
    if (destructor == _KEY_NONE) {
        pthread_mutex_unlock(&_key_mutex);
        return EINVAL;
    }
    /* find first free key */
    for (k = 0; k < PTHREAD_KEYS_MAX; k++) {
        if (_key_destructor[k] == _KEY_NONE) {
            break;
        }
    }

    /* this should never happen! */
    if (k >= PTHREAD_KEYS_MAX) {
        pthread_mutex_unlock(&_key_mutex);
        return EAGAIN;
    }

    _key_destructor[k] = destructor;
    *key = k;
    if (k >= _key_count)
        _key_count = k + 1;

    pthread_mutex_unlock(&_key_mutex);
    return EOK;
}
