/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * issetugid() — true when this image was started set-id by a spawner that is
 * not its owner. Roottask publishes that as an AT_RTMUX_SECURE auxv entry;
 * __rtmux_syschan_init() records where the auxiliary vector starts in
 * __libc_auxv_get(), for any start file that has set environ by then.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <sys/auxv.h>

int issetugid(void)
{
    static int cached = -1;

    if (cached < 0)
        cached = __libc_auxv_get(AT_RTMUX_SECURE) != 0;
    return cached;
}
