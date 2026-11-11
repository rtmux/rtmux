/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>
#include "cpucfg.h"

#undef pthread_self
pthread_t pthread_self(void)
{
    return (pthread_t) LIBC_TLS()->__tid;
}
