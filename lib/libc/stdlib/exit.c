/*
 * exit() — flush stdio, then _exit.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/cdefs.h>

extern void _cleanup(void) __hidden;

void exit(int status)
{
    _cleanup();
    _exit(status);
    __builtin_unreachable();
}
