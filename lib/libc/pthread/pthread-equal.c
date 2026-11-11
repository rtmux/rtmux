/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <pthread.h>

#undef pthread_equal
int pthread_equal(pthread_t t1, pthread_t t2)
{
    return t1 == t2;
}
