/*
 * pthread_kill() — send a signal to a thread.
 *
 * RTMux delivers signals process-wide from a single shared signal table (see
 * lib/libc/rtmux/signal-state.c); there is no per-thread signal target. A
 * pthread_kill() therefore collapses to process delivery, raising the signal in
 * the current process exactly as kill(getpid(), sig) does — so the handler runs
 * in the caller, not necessarily in `thread`. This is the documented divergence
 * from POSIX per-thread targeting forced by the process-global signal model.
 *
 * pthread_kill() targets only the caller's own process (POSIX forbids naming a
 * thread in another process), and RTMux keeps no libc-side registry of live
 * thread ids to validate against. A non-positive id is never a valid thread, so
 * it is rejected with ESRCH; any positive id is accepted as an in-process
 * thread and the signal is delivered process-wide. sig == 0 performs the usual
 * validity probe without sending a signal.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>

#include "signal-state.h"

int pthread_kill(pthread_t thread, int sig)
{
    if (thread <= 0)
        return ESRCH;
    if (sig < 0 || sig > MAXSIG)
        return EINVAL;
    if (sig == 0) /* existence probe: deliver nothing */
        return 0;
    if (__libc_signal_raise(sig) < 0)
        return errno;
    return 0;
}
