/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * issetugid() — true when this image was started set-id by a spawner that is
 * not its owner. Roottask publishes that as an AT_RTMUX_SECURE auxv entry;
 * __rtmux_syschan_init() records where the auxiliary vector starts in
 * __libc_auxv, for any start file that has set environ by then.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdint.h>
#include <unistd.h>
#include <sys/auxv.h>

extern const uint64_t *__libc_auxv __hidden;

int issetugid(void)
{
    static int cached = -1;
    const uint64_t *av;

    if (cached >= 0)
        return cached;
    cached = 0;
    av = __libc_auxv;
    for (; av && av[0] != AT_NULL; av += 2) {
        if (av[0] == AT_RTMUX_SECURE && av[1])
            cached = 1;
    }
    return cached;
}
