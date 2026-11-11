/*
 * console.c — handle _SYS_CONSOLE message.
 *
 * Updates the /dev/console symlink to point to a new device path. Called when a
 * real console driver (e.g. dev-serial-8250) is ready and the system should
 * switch from the boot console (devtext) to the interrupt-driven driver.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include <sys/path-registry.h>

/* Handle a _SYS_CONSOLE message: repoint /dev/console at a new device.
 *
 * Reads the new device path from the inline message body, removes the existing
 * /dev/console symlink, and creates a new one pointing at the requested path.
 * Parameters:
 *   ctp : the message context
 *   msg : the console message (path follows the header inline)
 * Return value: EOK on success, or an errno (EINVAL, EFAULT, or the
 *               symlink errno).
 */
int roottask_console_msg(ioserv_context_t *ctp, sys_console_t *msg)
{
    char path[PATH_MAX];
    int pathlen;

    /* Repointing /dev/console is integrity-sensitive: a hijacked console
     * captures the login / root tty. This handler had no sender check, so any
     * process could send _SYS_CONSOLE. Gate it root-only, the same way
     * _SYS_CMD_REBOOT/SHUTDOWN are — the legitimate switch runs as root.
     */
    if (!proc_isaccess(0, &ctp->info))
        return EPERM;

    pathlen = (int) msg->i.pathlen;
    if (pathlen <= 0 || pathlen > PATH_MAX)
        return EINVAL;

    /* The full message (header + path) was delivered inline by msg_receive —
     * "msg" points at the dispatcher's receive buffer, which is sized for the
     * largest proc_msg_union member, so the path sits right after the
     * _sys_console header. Read it from there instead of round-tripping through
     * msg_read, which has been intermittently returning EFAULT on the fast
     * BUFF_MSG path for small messages.
     */
    if ((unsigned) ctp->info.msglen <
        sizeof(struct _sys_console) + (unsigned) pathlen)
        return EFAULT;
    memcpy(path, (const char *) msg + sizeof(struct _sys_console),
           (size_t) pathlen);

    path[pathlen - 1] = '\0';

    /* Remove old symlink and create new one */
    unlink("/dev/console");

    if (namespace_symlink(path, "/dev/console") == -1) {
        root_log("roottask_console: symlink %s -> /dev/console failed\n", path);
        return errno;
    }

    root_log("system console set to %s\n", path);
    return EOK;
}
