/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Adapted for RTMux. */

#include <unistd.h>
#include <time.h>
#include <sys/rtmux-core.h>
#include <inttypes.h>
#include "signal-state.h"

unsigned sleep(unsigned seconds)
{
    uint64_t it, ot = 0;
    libc_waiter_t w;

    /* __timer_timeout writes the remaining time into *otime on the normal
     * expiry and signal-unblock paths, but a failure before the sleep blocks
     * (timer allocation, pointer probe, or a pending cancellation) returns
     * without writing it. Default to 0 so that rare case reports "nothing
     * remaining" instead of returning stack garbage.
     */
    it = seconds * (uint64_t) 1000000000;
    __libc_wait_enter(&w);
    (void) __timer_timeout(CLOCK_REALTIME, RTMUX_TIMEOUT_NANOSLEEP, NULL, &it,
                           &ot);
    __libc_wait_exit(&w);
    return (unsigned) (ot / (uint64_t) 1000000000);
}
