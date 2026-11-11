/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <errno.h>
#include <sys/iomsg.h>

#include <posix.h>

int close(int fd)
{
    io_close_t msg;
    int ret, err;

    msg.i.type = _IO_CLOSE;
    msg.i.combine_len = sizeof msg.i;
    if ((ret = msg_send(fd, &msg.i, sizeof msg.i, 0, 0)) != -1 ||
        errno != EINTR) {
        /* Drop any tracked *at path BEFORE detaching, while this fd number is
         * still reserved -- otherwise a concurrent open() could reuse the
         * number and this forget would clear the new mapping.
         */
        __atfd_forget(fd);
        if ((err = connect_detach_r((uint64_t) fd)) == EOK)
            return ret;
        if (ret != -1) {
            err = err < 0 ? -err : err;
            errno = err;
        }
    }
    return -1;
}
