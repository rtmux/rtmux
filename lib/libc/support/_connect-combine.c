/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <fcntl.h>
#include <sys/rtmux-core.h>
#include <sys/iomsg.h>
#include "connect.h"

int _connect_combine(const char *path,
                     mode_t mode,
                     unsigned oflag,
                     unsigned sflag,
                     int testcancel,
                     unsigned file_type,
                     unsigned extra_len,
                     void *extra,
                     unsigned response_len,
                     void *response)
{
    int fd, status;

    fd = _connect(RTMUX_SIDE_CHANNEL, path, mode, oflag, sflag,
                  _IO_CONNECT_COMBINE_CLOSE, testcancel, 0, file_type, 0,
                  extra_len, extra, response_len, response, &status);
    if (fd == -1)
        return -1;
    __connect_detach((uint64_t) fd);
    return status;
}
