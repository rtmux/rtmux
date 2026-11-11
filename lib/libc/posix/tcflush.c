/*
 * RTMux libc — tcflush(): discard pending terminal input and/or output.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <termios.h>
#include <devctl.h>
#include <devctl/tty.h>
#include <errno.h>
#include <unistd.h>

int tcflush(int fd, int queue_selector)
{
    if (queue_selector != TCIFLUSH && queue_selector != TCOFLUSH &&
        queue_selector != TCIOFLUSH) {
        errno = EINVAL;
        return -1;
    }

    /* Validate the descriptor first — EBADF for a bad fd, ENOTTY for a valid
     * non-terminal — matching POSIX error precedence and the other tc* covers.
     */
    if (!isatty(fd))
        return -1;
    return _devctl(fd, DEVCTL_TTY_FLUSH, &queue_selector, sizeof queue_selector,
                   _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY);
}
