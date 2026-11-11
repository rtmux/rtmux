/*
 * exit() — flush stdio, then _exit.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/cdefs.h>

extern void (*__cleanup)(void) __hidden;

void exit(int status)
{
    if (__cleanup)
        __cleanup();
    _exit(status);
    __builtin_unreachable();
}
