/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sched.h>
#include <sys/rtmux-core.h>


int sched_yield(void)
{
    return __sched_yield();
}
