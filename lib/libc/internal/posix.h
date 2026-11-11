/*
 * RTMux is freely redistributable under the Apache License. See the file
 * "LICENSE" for information on usage and redistribution of this file.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/* Private declarations shared by libc POSIX wrappers. */

#ifndef _RTMUX_LIBC_POSIX_H
#define _RTMUX_LIBC_POSIX_H

#include <stddef.h>
#include <signal.h>
#include <sys/cdefs.h>
#include <sys/poll.h>
#include <sys/stat.h>

int __stat_check(const struct stat *st) __hidden;
int __rtmux_openfd_typed(int fd, int oflag, int sflag, int xtype) __hidden;
int __rtmux_procfd_open(const char *path,
                        int oflag,
                        int sflag,
                        int *matched) __hidden;

/* fd-relative *at support: a descriptor -> absolute-path registry (atfd.c). */
void __atfd_track(int fd, const char *abspath) __hidden;
void __atfd_track_open(int fd, const char *path) __hidden;
void __atfd_forget(int fd) __hidden;
void __atfd_dup(int oldfd, int newfd) __hidden;
void __atfd_chdir(const char *abspath) __hidden;
void __atfd_chroot(void) __hidden;
int __atfd_abspath(int dirfd,
                   const char *path,
                   char *out,
                   size_t outsz) __hidden;
int __poll_pselect(struct pollfd *fds,
                   nfds_t nfds,
                   int timeout_ms,
                   const sigset_t *sigmask) __hidden;

#endif /* _RTMUX_LIBC_POSIX_H */
