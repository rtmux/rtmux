/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/procmsg.h>

pid_t rtmux_spawnp(const char *file,
                   int fd_count,
                   const int fd_map[],
                   const struct inheritance *inherit,
                   char *const argv[],
                   char *const envp[])
{
    struct inheritance attr;

    if (inherit)
        attr = *inherit;
    else
        memset(&attr, 0x00, sizeof attr);

    if (!strchr(file, '/'))
        attr.flags |= SPAWN_SEARCH_PATH;

    attr.flags |= SPAWN_CHECK_SCRIPT;

    return rtmux_spawn(file, fd_count, fd_map, &attr, argv, envp);
}
