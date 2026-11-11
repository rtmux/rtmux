/*
 * sigprocmask() — POSIX signal mask manipulation.
 *
 * Thin shim over the libc-internal signal table. See
 * doc/md/libc_signals_two_thread.md and lib/libc/rtmux/signal-state.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>

#include "signal-state.h"

int sigprocmask(int how, const sigset_t *set, sigset_t *oset)
{
    return __libc_sigprocmask(how, set, oset);
}
