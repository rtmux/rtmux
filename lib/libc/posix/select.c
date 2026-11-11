/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <sys/poll.h>
#include <sys/select.h>
#include <sys/time.h>

#include <posix.h>

/* Convert a select timeval to a poll millisecond count, rounding a sub-
 * millisecond remainder up so a tiny timeout never truncates to a busy 0.
 * Returns -1 (EINVAL) on a malformed value.
 */
static int select_timeout_ms(const struct timeval *tv)
{
    long long ms;

    if (tv->tv_sec < 0 || tv->tv_usec < 0 || tv->tv_usec >= 1000000)
        return -1;
    if (tv->tv_sec >= INT_MAX / 1000) {
        ms = INT_MAX;
    } else {
        ms = (long long) tv->tv_sec * 1000 + (tv->tv_usec + 999) / 1000;
        if (ms > INT_MAX)
            ms = INT_MAX;
    }
    return (int) ms;
}

/* select() over poll(): translate the three descriptor sets into a pollfd
 * array, wait with poll(), then translate the returned events back. A
 * descriptor is read-ready on POLLIN/POLLHUP/POLLERR, write-ready on
 * POLLOUT/POLLERR, and exception-ready on POLLPRI; an invalid descriptor
 * (POLLNVAL) fails the whole call with EBADF, as POSIX requires. The return
 * value is the total number of ready (descriptor, condition) pairs, so a
 * descriptor ready for two conditions counts twice.
 */
static int select_poll(int nfds,
                       fd_set *readfds,
                       fd_set *writefds,
                       fd_set *exceptfds,
                       int timeout_ms,
                       const sigset_t *sigmask)
{
    struct pollfd *pfds;
    int count, n, i, ready, fd;

    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    count = 0;
    for (fd = 0; fd < nfds; fd++) {
        if ((readfds && FD_ISSET(fd, readfds)) ||
            (writefds && FD_ISSET(fd, writefds)) ||
            (exceptfds && FD_ISSET(fd, exceptfds)))
            count++;
    }

    pfds = count ? malloc((size_t) count * sizeof *pfds) : NULL;
    if (count && !pfds) {
        errno = ENOMEM;
        return -1;
    }

    n = 0;
    for (fd = 0; fd < nfds; fd++) {
        short events = 0;

        if (readfds && FD_ISSET(fd, readfds))
            events |= POLLIN;
        if (writefds && FD_ISSET(fd, writefds))
            events |= POLLOUT;
        if (exceptfds && FD_ISSET(fd, exceptfds))
            events |= POLLPRI;
        if (!events)
            continue;
        pfds[n].fd = fd;
        pfds[n].events = events;
        pfds[n].revents = 0;
        n++;
    }

    if (__poll_pselect(pfds, (nfds_t) n, timeout_ms, sigmask) == -1) {
        free(pfds);
        return -1;
    }

    /* Verify validity of descriptors before altering user-supplied fd_sets */
    for (i = 0; i < n; i++) {
        if (pfds[i].revents & POLLNVAL) {
            free(pfds);
            errno = EBADF;
            return -1;
        }
    }

    if (readfds)
        FD_ZERO(readfds);
    if (writefds)
        FD_ZERO(writefds);
    if (exceptfds)
        FD_ZERO(exceptfds);

    ready = 0;
    for (i = 0; i < n; i++) {
        short re = pfds[i].revents;
        fd = pfds[i].fd;
        if (readfds && (re & (POLLIN | POLLHUP | POLLERR))) {
            FD_SET(fd, readfds);
            ready++;
        }
        if (writefds && (re & (POLLOUT | POLLERR))) {
            FD_SET(fd, writefds);
            ready++;
        }
        if (exceptfds && (re & POLLPRI)) {
            FD_SET(fd, exceptfds);
            ready++;
        }
    }

    free(pfds);
    return ready;
}

int select(int nfds,
           fd_set *readfds,
           fd_set *writefds,
           fd_set *exceptfds,
           struct timeval *timeout)
{
    int timeout_ms;

    /* A non-NULL timeout only yields -1 from select_timeout_ms when malformed;
     * a NULL timeout means block indefinitely (poll timeout -1).
     */
    timeout_ms = timeout ? select_timeout_ms(timeout) : -1;
    if (timeout && timeout_ms == -1) {
        errno = EINVAL;
        return -1;
    }

    return select_poll(nfds, readfds, writefds, exceptfds, timeout_ms, NULL);
}

/* pselect(): select() with a nanosecond-input timeout and an atomic temporary
 * process-wide signal-mask swap around the poll-backed wait.
 */
int pselect(int nfds,
            fd_set *readfds,
            fd_set *writefds,
            fd_set *exceptfds,
            const struct timespec *timeout,
            const sigset_t *sigmask)
{
    int timeout_ms = -1;

    if (nfds < 0 || nfds > FD_SETSIZE) {
        errno = EINVAL;
        return -1;
    }

    if (timeout) {
        if (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
            timeout->tv_nsec >= 1000000000L) {
            errno = EINVAL;
            return -1;
        }
        if (timeout->tv_sec >= INT_MAX / 1000) {
            timeout_ms = INT_MAX;
        } else {
            long long ms = (long long) timeout->tv_sec * 1000 +
                           (timeout->tv_nsec + 999999L) / 1000000L;
            timeout_ms = ms > INT_MAX ? INT_MAX : (int) ms;
        }
    }

    return select_poll(nfds, readfds, writefds, exceptfds, timeout_ms, sigmask);
}
