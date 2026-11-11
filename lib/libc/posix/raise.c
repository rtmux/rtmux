/*
 * raise() — synchronously raise a signal in the calling thread.
 *
 * Runs the registered handler directly from the calling thread, before
 * returning. kill(getpid(), sig) uses the same in-process path.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>

#include "signal-state.h"

int raise(int sig)
{
    return __libc_signal_raise(sig);
}
