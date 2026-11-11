/*
 * dup() — duplicate file descriptor.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/rtmux-core.h>

int dup(int fd)
{
    return fcntl(fd, F_DUPFD, 0);
}

int dup2(int oldfd, int newfd)
{
    /* Validate oldfd exactly as F_DUPFD will — reject -1, side-channel handles,
     * and unknown endpoints — before touching newfd, so a failed dup2 leaves
     * newfd unchanged. (A bare F_GETFD would accept a side-channel oldfd that
     * F_DUPFD later rejects, after newfd had already been closed.) A bad newfd
     * is rejected here too: POSIX wants EBADF for an invalid fildes2, not
     * F_DUPFD's EINVAL.
     */
    if (oldfd == -1 || ((unsigned) oldfd & RTMUX_SIDE_CHANNEL) ||
        fcntl(oldfd, F_GETFD, 0) == -1 || newfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd)
        return newfd;
    close(newfd);
    return fcntl(oldfd, F_DUPFD, newfd);
}
