/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdlib.h>
#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/path-registry.h>
#include <sys/pathmsg.h>
#include "connect.h"

int _connect_fd(int base,
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
                int *fd_len,
                void *fd_array)
{
    struct _connect_ctrl ctrl;
    struct _io_connect msg;
    int fd;

    memset(&ctrl, 0x00, sizeof ctrl);
    ctrl.base = base;
    ctrl.extra = extra;
    ctrl.send = testcancel ? msg_sendv : msg_sendvnc;
    ctrl.msg = &msg;

    if (fd_array && fd_len) {
        if (!*fd_len) {
            ctrl.fds_len = FD_BUF_INCREMENT;
            ctrl.fds = malloc(ctrl.fds_len * sizeof(*ctrl.fds));
            ctrl.flags |= FLAG_MALLOC_FDS;
        } else if (*fd_len) {
            ctrl.fds = *(int **) fd_array;
            ctrl.fds_len = (uint16_t) *fd_len;
        }
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
    if (ctrl.flags & FLAG_MALLOC_FDS)
        *(int **) fd_array = ctrl.fds;
    if (fd_len)
        *fd_len = ctrl.fds_index;

    return fd;
}
