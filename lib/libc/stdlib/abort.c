/*
 * abort() — abnormal process termination.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <stdlib.h>
#include "signal-state.h"

void abort(void)
{
    sigset_t abrt;
    sigemptyset(&abrt);
    sigaddset(&abrt, SIGABRT);
    sigprocmask(SIG_UNBLOCK, &abrt, NULL);
    raise(SIGABRT);

    struct sigaction sa = {.sa_handler = SIG_DFL};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGABRT, &sa, NULL);
    sigprocmask(SIG_UNBLOCK, &abrt, NULL);
    raise(SIGABRT);
    __libc_signal_exit(SIGABRT);
}
