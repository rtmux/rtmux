/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "signal-state.h"

int pause(void)
{
    return __libc_signal_pause();
}
