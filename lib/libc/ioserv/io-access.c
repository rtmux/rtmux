/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/node.h>
#include <sys/rtmux-core.h>
#include <sys/ioserv-private.h>
#include "internal.h"

/* Decide whether the sender of an _IO_DUP/_IO_OPENFD may use the handle the
 * message names. Possession is the test, never uid equality:
 *   - the sender's own connection (same scoid);
 *   - roottask (session.c / devtty plumbing), identified by the kernel-supplied
 *     sender pid;
 *   - a key the proc or path service minted for this very thread over these
 *     exact bytes (spawn inheritance, /dev/tty), verified by msg_key_data.
 * Anything else is EACCES. Parameters:
 *   key1 : the key carried in the message; msg/size: the bytes it covers
 */
int _ioserv_access(ioserv_context_t *ctp,
                   struct _msg_info *msginfo,
                   unsigned key1,
                   void *msg,
                   int size)
{
    unsigned key2;

    if (ctp->info.scoid == msginfo->scoid ||
        (ctp->info.nd == RTMUX_LOCAL_NODE && ctp->info.pid == ROOTTASK_PID)) {
        return EOK;
    }

    if (msg && key1) {
        SETIOV(ctp->iov + 0, msg, size);

        if (msg_key_data_r(ctp->rcvid, RTMUX_KEYDATA_VERIFY, key1, &key2,
                           ctp->iov + 0, 1) == EOK &&
            key2 == 0) {
            return EOK;
        }
    }

    return EACCES;
}
