/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <fcntl.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
#include "connect.h"

int _connect_entry(int base,
                   const char *path,
                   mode_t mode,
                   unsigned oflag,
                   unsigned sflag,
                   unsigned subtype,
                   int testcancel,
                   unsigned accessl,
                   unsigned file_type,
                   unsigned extra_type,
                   unsigned extra_len,
                   const void *extra,
                   unsigned response_len,
                   void *response,
                   int *status,
                   struct _io_connect_entry *entry,
                   int enoretry)
{
    struct _connect_ctrl ctrl;
    struct _io_connect msg;
    int fd;

    memset(&ctrl, 0x00, sizeof ctrl);
    ctrl.base = base;
    ctrl.extra = extra;
    ctrl.send = testcancel ? msg_sendv : msg_sendvnc;
    ctrl.msg = &msg;
    if ((ctrl.entry = entry)) {
        ctrl.flags |= FLAG_TEST_ENTRY | (enoretry ? FLAG_NO_RETRY : 0);
    }

    memset(&msg, 0x00, sizeof msg);
    msg.subtype = (uint16_t) subtype;
    msg.sflag = (uint16_t) sflag;
    msg.ioflag = oflag;
    msg.mode = mode;
    msg.file_type = file_type;
    msg.access = (uint16_t) accessl;
    msg.extra_type = (uint8_t) extra_type;
    msg.extra_len = (uint16_t) extra_len;

    fd = _connect_ctrl(&ctrl, path, response_len, response);
    if (status)
        *status = ctrl.status;
    return fd;
}
