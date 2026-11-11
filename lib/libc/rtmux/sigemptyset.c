/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <errno.h>

int sigemptyset(sigset_t *set)
{
    set->bits[0] = set->bits[1] = 0;
    return 0;
}
