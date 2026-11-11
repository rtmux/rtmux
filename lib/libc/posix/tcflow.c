/*
 * RTMux libc — tcflow(): terminal flow control.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <termios.h>
#include <devctl.h>
#include <devctl/tty.h>
#include <errno.h>
#include <unistd.h>

int tcflow(int fd, int action)
{
    if (action != TCOOFF && action != TCOON && action != TCIOFF &&
        action != TCION) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(fd))
        return -1;
    return _devctl(fd, DEVCTL_TTY_FLOW, &action, sizeof action,
                   _DEVCTL_FLAG_NORETVAL);
}
