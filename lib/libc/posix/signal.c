/*
 * signal() — POSIX/BSD-style handler registration.
 *
 * Thin wrapper over sigaction(). Uses System-V semantics (SA_RESETHAND not set)
 * — this matches modern Linux/BSD glibc. Callers wanting one-shot semantics
 * should use sigaction() with SA_RESETHAND directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <string.h>

#include "signal-state.h"

void (*signal(int signo, void (*func)(int)))(int)
{
    struct sigaction act, oact;

    memset(&act, 0, sizeof(act));
    act.sa_handler = func;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;

    if (__libc_sigaction(signo, &act, &oact) < 0)
        return SIG_ERR;
    return oact.sa_handler;
}
