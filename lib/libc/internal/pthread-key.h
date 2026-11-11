/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _PTHREAD_KEY_INCLUDED
#define _PTHREAD_KEY_INCLUDED

#include <pthread.h>
#include <sys/cdefs.h>

#define _KEY_NONE ((_key_destructor_t) - 1)

typedef void (*_key_destructor_t)(void *);
typedef int _key_count_t;

extern _key_count_t _key_count __hidden;
extern _key_destructor_t *_key_destructor __hidden;
extern pthread_mutex_t _key_mutex __hidden;

extern void _key_delete(pthread_key_t) __hidden;

#endif
