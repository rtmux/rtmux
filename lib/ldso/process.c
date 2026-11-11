/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux ldso process shim implementations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rtmux-shim.h"

#include <sys/storage.h>

extern int thread_destroy_all_r(void);

void rtmux_ldso_exit(int status)
{
    (void) status;
    thread_destroy_all_r();
    for (;;)
        ;
}

pid_t rtmux_ldso_getpid(void)
{
    return __tls()->__pid;
}
