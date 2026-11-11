/*
 * RTMux libc — tcsendbreak(): send a break on an asynchronous serial line.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <devctl.h>
#include <devctl/tty.h>
#include <unistd.h>

int tcsendbreak(int fd, int duration)
{
    /* POSIX leaves the break length implementation-defined for a nonzero
     * duration and 0.25-0.5s for zero, so the driver picks the interval and the
     * argument is not forwarded. Validate the descriptor first — EBADF for a
     * bad fd, ENOTTY for a valid non-terminal — then ask the driver to break; a
     * terminal that is not an async serial line takes no action and succeeds.
     */
    (void) duration;
    if (!isatty(fd))
        return -1;

    /* isatty() already proved this is a terminal, so do NOT pass
     * _DEVCTL_FLAG_NOTTY: a driver that cannot generate a break (e.g. the
     * SiFive UART) returns ENOSYS, which must reach the caller as ENOSYS, not
     * be rewritten to ENOTTY on a genuine tty.
     */
    return _devctl(fd, DEVCTL_TTY_SENDBREAK, NULL, 0, _DEVCTL_FLAG_NORETVAL);
}
