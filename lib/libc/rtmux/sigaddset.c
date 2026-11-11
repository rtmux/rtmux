/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <signal.h>
#include <errno.h>

int sigaddset(sigset_t *set, int signo)
{
    if ((unsigned long) --signo >= (unsigned long) MAXSIG) {
        errno = EINVAL;
        return -1;
    }
#define BITS_SIZE ((unsigned) sizeof(set->bits[0]) * 8)

    set->bits[(unsigned) signo / BITS_SIZE] |=
        1U << ((unsigned) signo % BITS_SIZE);

    return 0;
}
