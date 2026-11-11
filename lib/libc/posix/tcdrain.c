/*
 * RTMux libc — tcdrain(): wait until all written terminal output is sent.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <devctl.h>
#include <devctl/tty.h>
#include <unistd.h>

int tcdrain(int fd)
{
    /* Validate the descriptor first — EBADF for a bad fd, ENOTTY for a valid
     * non-terminal — matching POSIX error precedence and the other tc* covers,
     * then let the driver block until its transmitter is idle.
     */
    if (!isatty(fd))
        return -1;
    return _devctl(fd, DEVCTL_TTY_DRAIN, NULL, 0,
                   _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY);
}
