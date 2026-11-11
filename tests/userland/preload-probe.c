/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * preload-probe — a real LD_PRELOAD object for the loader tests. It interposes
 * getegid(): a probe that sees PRELOAD_PROBE_EGID instead of its real group id
 * proves the loader preloaded the object ahead of libc, and because the
 * replacement itself calls getpid() the object's own undefined libc reference
 * must resolve too. It is linked with lld, as libc.so is.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/rtmux-ident.h>
#include <sys/types.h>
#include <unistd.h>

RTMUX_IDENT(RTMUX_TYPE_SHLIB, 0, 1);

#define PRELOAD_PROBE_EGID 7001

gid_t getegid(void)
{
    return getpid() > 0 ? PRELOAD_PROBE_EGID : 0;
}
