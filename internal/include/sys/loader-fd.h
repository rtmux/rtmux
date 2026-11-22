/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RTMUX_LOADER_FD_H
#define RTMUX_LOADER_FD_H
#include <stddef.h>
#include <sys/cdefs.h>

/* The loader runs in a child while sharing the roottask libc image.
 * Its descriptor operations must bypass the roottask path cache. */
char *__loader_root(char *buf, size_t size) __hidden;
int __loader_open(const char *path, int flags, ...) __hidden;
int __close_fd(int fd, int track) __hidden;
int __dup_fd(int fd, int minfd, unsigned flags, int track) __hidden;
int __atfd_fdinfo_path(int fd, char *path, size_t cap) __hidden;
int __chdir_resolve(const char *path, char *out, size_t cap) __hidden;
#endif
