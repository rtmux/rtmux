/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include "pthread-key.h"

int pthread_key_delete(pthread_key_t key)
{
    if (key < 0 || (unsigned) key >= PTHREAD_KEYS_MAX || !_key_destructor)
        return EINVAL;

    if (pthread_mutex_lock(&_key_mutex) != EOK)
        return EINVAL;
    if (_key_destructor[key] == _KEY_NONE) {
        pthread_mutex_unlock(&_key_mutex);
        return EINVAL;
    }
    _key_destructor[key] = _KEY_NONE;
    while (_key_count > 0 &&
           _key_destructor[(pthread_key_t) _key_count - 1] == _KEY_NONE)
        _key_count--;
    _key_delete(key);
    pthread_mutex_unlock(&_key_mutex);
    return EOK;
}
