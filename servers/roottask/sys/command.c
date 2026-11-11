/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "externs.h"
#include "root-log.h"

/* Handle a _SYS_CMD message: system reboot, shutdown, sync, or log mark.
 *
 * Reboot and shutdown require the sender to be root and flush every mount
 * first unless _SYS_CMD_FLAG_NOSYNC is set; sync is open to everyone. A log
 * mark (root only) appends up to ROOT_LOG_MARK_MAX bytes of text to the ring,
 * control bytes replaced by '?'.
 * Parameters:
 *   ctp : the message context
 *   msg : the system-command message
 * Return value: EPERM if the caller lacks permission, EINVAL for a malformed
 *               log mark, EOK for sync and log mark, ENOSYS for an
 *               unrecognized command (reboot/shutdown do not return).
 */
int roottask_command(ioserv_context_t *ctp, sys_cmd_t *msg)
{
    int nosync;

    if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF) {
        ENDIAN_SWAP16(&msg->i.cmd);
    }
    /* Older senders stop after the cmd field. */
    nosync = ctp->size >= (int) sizeof msg->i &&
             (msg->i.flags & _SYS_CMD_FLAG_NOSYNC);
    switch (msg->i.cmd) {
    case _SYS_CMD_REBOOT:
        if (!proc_isaccess(0, &ctp->info)) {
            return EPERM;
        }
        if (!nosync)
            (void) root_sync_all(0);
        system_reboot(0);
        break;
    case _SYS_CMD_SHUTDOWN:
        if (!proc_isaccess(0, &ctp->info)) {
            return EPERM;
        }
        if (!nosync)
            (void) root_sync_all(0);
        system_shutdown();
        break;
    case _SYS_CMD_SYNC:
        return root_sync_all(1);
    case _SYS_CMD_LOG_MARK: {
        char text[ROOT_LOG_MARK_MAX + 1];
        size_t len;

        if (!proc_isaccess(0, &ctp->info))
            return EPERM;
        /* info.msglen is the bytes received; ctp->size is not set for this path. */
        if (ctp->info.msglen < sizeof msg->mark)
            return EINVAL;
        if (ctp->info.flags & RTMUX_MI_ENDIAN_DIFF)
            ENDIAN_SWAP16(&msg->mark.len);
        len = msg->mark.len;
        if (len > ROOT_LOG_MARK_MAX || ctp->info.msglen < sizeof msg->mark + len)
            return EINVAL;
        memcpy(text, &msg->mark + 1, len);
        text[len] = '\0';
        /* One mark is one ring line: a control byte (newline, ESC, NUL) would forge
         * extra lines or console escapes, so each becomes '?'.
         */
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char) text[i] < 0x20 || text[i] == 0x7f)
                text[i] = '?';
        }
        root_log("%s", text); /* text is data, never a format */
        return EOK;
    }
    default:
        break;
    }
    return ENOSYS;
}
