/*
 * sigaction() — POSIX signal action.
 *
 * Thin shim over the libc-internal signal table. See
 * doc/md/libc_signals_two_thread.md and lib/libc/rtmux/signal-state.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>

#include "signal-state.h"

int sigaction(int signo, const struct sigaction *act, struct sigaction *oact)
{
    return __libc_sigaction(signo, act, oact);
}
