/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * (c) 2026 RTMux contributors
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef _SYS_SELECT_H
#define _SYS_SELECT_H

#include <sys/time.h> /* struct timeval */
#include <time.h>     /* struct timespec */
#include <signal.h>   /* sigset_t */

#ifndef FD_SETSIZE
#define FD_SETSIZE 1024
#endif

typedef unsigned long __fd_mask;

#define __NFDBITS ((int) (8 * sizeof(__fd_mask)))
#define __FD_ELT(fd) ((fd) / __NFDBITS)
#define __FD_MASK(fd) ((__fd_mask) 1UL << ((unsigned) (fd) % __NFDBITS))

typedef struct {
    __fd_mask fds_bits[FD_SETSIZE / __NFDBITS];
} fd_set;

#define FD_ZERO(set)                                       \
    do {                                                   \
        fd_set *__s = (set);                               \
        int __i;                                           \
        for (__i = 0; __i < FD_SETSIZE / __NFDBITS; __i++) \
            __s->fds_bits[__i] = 0;                        \
    } while (0)
#define FD_SET(fd, set) ((set)->fds_bits[__FD_ELT(fd)] |= __FD_MASK(fd))
#define FD_CLR(fd, set) ((set)->fds_bits[__FD_ELT(fd)] &= ~__FD_MASK(fd))
#define FD_ISSET(fd, set) (((set)->fds_bits[__FD_ELT(fd)] & __FD_MASK(fd)) != 0)

__BEGIN_DECLS
int select(int __nfds,
           fd_set *__readfds,
           fd_set *__writefds,
           fd_set *__exceptfds,
           struct timeval *__timeout);
int pselect(int __nfds,
            fd_set *__readfds,
            fd_set *__writefds,
            fd_set *__exceptfds,
            const struct timespec *__timeout,
            const sigset_t *__sigmask);
__END_DECLS

#endif /* _SYS_SELECT_H */
