/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include "pthread-key.h"

_key_count_t _key_count = 0;
_key_destructor_t *_key_destructor = NULL;
pthread_mutex_t _key_mutex = PTHREAD_MUTEX_INITIALIZER;
