/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include "cpucfg.h"

#undef pthread_getspecific
void *pthread_getspecific(pthread_key_t key)
{
    if (key < LIBC_TLS()->__numkeys)
        return LIBC_TLS()->__keydata[key];
    return NULL;
}
