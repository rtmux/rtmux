/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * roottask_console_set() — set the system console device.
 *
 * Sends _SYS_CONSOLE to roottask, which updates the /dev/console symlink to
 * point to the given path.
 *
 * License: Apache 2.0
 */

#include <errno.h>
#include <string.h>
#include <sys/rtmux-core.h>
#include <sys/sysmsg.h>
#include <sys/iomsg.h>

int roottask_console_set(const char *path)
{
    struct {
        struct _sys_console hdr;
        char path[PATH_MAX];
    } msg;
    unsigned len;

    if (!path)
        return EINVAL;

    len = (unsigned) strlen(path) + 1;
    if (len > PATH_MAX)
        return ENAMETOOLONG;

    msg.hdr.type = _SYS_CONSOLE;
    msg.hdr.pathlen = (uint16_t) len;
    memcpy(msg.path, path, len);

    return msg_sendnc(ROOTTASK_COID, &msg, sizeof(msg.hdr) + len, 0, 0);
}
