/*
 * RTMux libc — short-term POSIX function stubs.
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The functions here are placeholder implementations sufficient to make
 * programs that *reference* them link cleanly. Each one needs a real
 * implementation that talks to roottask / proc service / the kernel sooner or
 * later. Marked TODO at every site so they're easy to find and graduate.
 *
 * Surfaced by POSIX userland ports.
 */

#include <errno.h>
#include <stdarg.h>
#include <devctl.h>
#include <sys/ioctl.h>

#define IOCTL_PARAM_MASK 0x3fffU

static size_t ioctl_param_len(int request)
{
    return ((unsigned) request >> 16) & IOCTL_PARAM_MASK;
}

/* ioctl — generic terminal/device control over the native devctl wire format.
 */
int ioctl(int fd, int request, ...)
{
    void *data = NULL;

    size_t nbytes = ioctl_param_len(request);

    if (nbytes > 0) {
        va_list ap;

        va_start(ap, request);
        data = va_arg(ap, void *);
        va_end(ap);
    }

    return _devctl(fd, (unsigned) request, data, nbytes,
                   _DEVCTL_FLAG_NORETVAL | _DEVCTL_FLAG_NOTTY);
}
