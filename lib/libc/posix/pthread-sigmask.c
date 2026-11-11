/*
 * pthread_sigmask() — examine and change the signal mask.
 *
 * RTMux keeps a single process-wide signal mask shared by all threads (see
 * lib/libc/rtmux/signal-state.c), so pthread_sigmask() and sigprocmask()
 * operate on the same mask. This matches POSIX exactly for a single-threaded
 * process; with multiple threads the mask is process-global rather than
 * per-thread — the documented divergence forced by RTMux's process-global
 * signal model. Unlike sigprocmask(), which reports failure through errno, the
 * pthread_* functions return the error number directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include "signal-state.h"

int pthread_sigmask(int how, const sigset_t *set, sigset_t *oset)
{
    if (__libc_sigprocmask(how, set, oset) < 0)
        return errno;
    return 0;
}
