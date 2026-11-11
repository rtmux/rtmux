/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * setuid-probe — reports, through its exit status, what a set-id image sees:
 *   bit 3: main() ran (the dynamic loader accepted the environment)
 *   bit 1: issetugid() is true
 *   bit 2: the effective uid is 0
 *   bit 4: the effective gid is 0
 *   bit 5: getegid() is the one interposed by an LD_PRELOAD of preload-probe.so
 * Installed three times: setuid-probe (mode 4755), setgid-probe (2755) and
 * setuid-probe-plain (0755).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <sys/types.h>
#include <unistd.h>

/* getegid() as interposed by preload-probe.so (tests/userland/preload-probe.c) */
#define PRELOAD_PROBE_EGID 7001

int main(void)
{
    gid_t egid = getegid();

    return 8 | (issetugid() ? 2 : 0) | (geteuid() == 0 ? 4 : 0) |
           (egid == 0 ? 16 : 0) | (egid == PRELOAD_PROBE_EGID ? 32 : 0);
}
