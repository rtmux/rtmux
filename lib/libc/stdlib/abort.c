/*
 * abort() — abnormal process termination.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

void abort(void)
{
    raise(SIGABRT);
    _exit(128 + SIGABRT);
    __builtin_unreachable();
}
