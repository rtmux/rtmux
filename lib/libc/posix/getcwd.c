/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <sys/iomsg.h>
#include "connect.h"

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

char *getcwd(char *buf, size_t size)
{
    struct _connect_ctrl ctrl;
    struct _io_connect msg;
    int fd;
    size_t len;
    char *save;
    int saved_errno;

    if (buf && size <= 0) {
        errno = EINVAL;
        return 0;
    }

    if (!(save = buf) && !(buf = malloc(size = PATH_MAX + 1))) {
        errno = ENOMEM;
        return 0;
    }

    memset(&ctrl, 0x00, sizeof ctrl);
    ctrl.base = RTMUX_SIDE_CHANNEL;
    ctrl.path = buf;
    ctrl.pathsize = (int) size;
    ctrl.flags = _IO_CONNECT_RET_CHROOT;
    ctrl.flags |= FLAG_NO_SYM;
    ctrl.send = msg_sendvnc;
    ctrl.msg = &msg;

    memset(&msg, 0x00, sizeof msg);
    msg.ioflag = O_NOCTTY;
    msg.subtype = _IO_CONNECT_COMBINE_CLOSE;

    saved_errno = errno;
    if ((fd = _connect_ctrl(&ctrl, ".", 0, 0)) == -1) {
        if (!save)
            free(buf);
        return 0;
    }
    errno = saved_errno;
    __connect_detach((uint64_t) fd);

    if (buf[0] == '\0') {
        errno = ERANGE;
        if (!save)
            free(buf);
        return 0;
    }

    /* Return path relative to last chroot */
    if (ctrl.chroot_len > 0) {
        len = strlen(buf);
        if (ctrl.chroot_len == len) {
            if (size < 2) {
                errno = ERANGE;
                if (!save)
                    free(buf);
                return 0;
            }
            buf[0] = '/';
            buf[1] = '\0';
        } else {
            memmove(buf, buf + ctrl.chroot_len, (len + 1) - ctrl.chroot_len);
        }
    }

    return buf;
}
