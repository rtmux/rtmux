/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * RTMux ldso signal shim implementations.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "rtmux-shim.h"

#include <string.h>

#include <sys/storage.h>

int rtmux_ldso_sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    (void) how;
    (void) set;
    if (oset)
        memset(oset, 0, sizeof(*oset));
    return 0;
}

int rtmux_ldso_thr_self(long *idp)
{
    if (idp)
        *idp = (long) __tls()->__tid;
    return 0;
}

int rtmux_ldso_thr_kill(long tid, int sig)
{
    (void) tid;
    (void) sig;
    return 0;
}
