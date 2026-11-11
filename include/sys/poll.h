/**
 * \file poll.h
 *
 * Definitions and prototypes for poll().
 *
 * \copyright (c) 1998 The NetBSD Foundation, Inc. All rights reserved.
 * \author Charles M. Hannum and other NetBSD contributors. \license NetBSD
 *         4-clause
 *
 * SPDX-License-Identifier: BSD-4-Clause
 */

#ifndef _SYS_POLL_H
#define _SYS_POLL_H

typedef unsigned int nfds_t;

struct pollfd {
    int fd;        /* file descriptor */
    short events;  /* events to look for */
    short revents; /* events returned */
};

/* Testable events (may be specified in events field). */
#define POLLRDNORM 0x0001
#define POLLOUT 0x0002
#define POLLWRNORM POLLOUT
#define POLLRDBAND 0x0004
#define POLLIN (POLLRDNORM | POLLRDBAND)
#define POLLPRI 0x0008
#define POLLWRBAND 0x0010

/* Non-testable events (may not be specified in events field). */
#define POLLERR 0x0020
#define POLLHUP 0x0040
#define POLLNVAL 0x1000

#ifndef _KERNEL

__BEGIN_DECLS
int poll(struct pollfd *, nfds_t, int);
__END_DECLS

#endif /* _KERNEL */

#endif
